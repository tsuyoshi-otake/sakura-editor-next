[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [ValidateRange(1, 1000)]
    [int]$Samples = 10,
    [ValidateRange(1, 100000)]
    [int]$FramesPerSample = 500,
    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$schemaVersion = 1
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$binaryPath = Join-Path $repoRoot ("x64\{0}\tests1.exe" -f $Configuration)
$outputRoot = [System.IO.Path]::GetFullPath($OutputDirectory)
$requiredBackends = @('legacy-gdi', 'gdi-plus', 'directwrite', 'directwrite-d2d', 'candidate-native')
$requiredCorpora = @('ascii-shell', 'tui-box-block-shade', 'mixed-unicode')
$requiredDpis = @(96, 120, 144, 192)
$counterNames = @(
    'frames', 'extTextOutCalls', 'gdiBatchCount', 'drawStringCalls',
    'textLayoutCreates', 'fallbackRuns', 'fallbackCacheHits',
    'fallbackCacheMisses', 'mapCharactersCalls', 'analysisCalls', 'glyphsCalls',
    'placementCalls', 'd2dTargetCreates', 'd2dTargetBinds',
    'd2dDrawCalls', 'd2dEndDrawCalls', 'd2dTargetLosses'
)

if (-not (Test-Path -LiteralPath $binaryPath -PathType Leaf)) {
    throw "tests1.exe was not found: $binaryPath. Build the requested configuration first."
}
New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null
$runDirectory = Join-Path $outputRoot ("runs-v{0}-{1:yyyyMMdd-HHmmssfff}-{2}" -f $schemaVersion, (Get-Date), $PID)
New-Item -ItemType Directory -Path $runDirectory -Force | Out-Null

function Get-Median {
    param([double[]]$Values)
    if ($null -eq $Values -or $Values.Count -eq 0) {
        throw 'Cannot calculate a median from an empty sample.'
    }
    $ordered = @($Values | Sort-Object)
    $middle = [int]($ordered.Count / 2)
    if (($ordered.Count % 2) -eq 1) {
        return [double]$ordered[$middle]
    }
    return ([double]$ordered[$middle - 1] + [double]$ordered[$middle]) / 2.0
}

function Get-P95 {
    param([double[]]$Values)
    if ($null -eq $Values -or $Values.Count -eq 0) {
        throw 'Cannot calculate p95 from an empty sample.'
    }
    $ordered = @($Values | Sort-Object)
    $index = [int][Math]::Ceiling($ordered.Count * 0.95) - 1
    $index = [Math]::Max(0, [Math]::Min($ordered.Count - 1, $index))
    return [double]$ordered[$index]
}

function Stop-BaselineProcessTree {
    param([System.Diagnostics.Process]$Process)
    if ($null -eq $Process) {
        return
    }
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
        if (-not $Process.HasExited) {
            $Process.WaitForExit(10000) | Out-Null
        }
    } catch {}
}

function Invoke-BaselineChild {
    param(
        [string]$Backend,
        [string]$Corpus,
        [int]$Dpi,
        [int]$SampleIndex,
        [string]$JsonPath
    )
    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $binaryPath
    $startInfo.Arguments = '--gtest_filter=TerminalLegacyRendererBaseline.EmitsConfiguredBenchmarkJson'
    $startInfo.WorkingDirectory = $repoRoot
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.Environment['SAKURA_TERMINAL_BASELINE_OUTPUT'] = $JsonPath
    $startInfo.Environment['SAKURA_TERMINAL_BASELINE_BACKEND'] = $Backend
    $startInfo.Environment['SAKURA_TERMINAL_BASELINE_CORPUS'] = $Corpus
    $startInfo.Environment['SAKURA_TERMINAL_BASELINE_DPI'] = [string]$Dpi
    $startInfo.Environment['SAKURA_TERMINAL_BASELINE_WARMUP_FRAMES'] = '2'
    $startInfo.Environment['SAKURA_TERMINAL_BASELINE_FRAMES'] = [string]$FramesPerSample
    $startInfo.Environment['SAKURA_TERMINAL_BASELINE_COLUMNS'] = '120'
    $startInfo.Environment['SAKURA_TERMINAL_BASELINE_ROWS'] = '8'

    $process = [System.Diagnostics.Process]::new()
    try {
        $process.StartInfo = $startInfo
        if (-not $process.Start()) {
            throw "Unable to start tests1.exe for $Backend/$Corpus/$Dpi sample $SampleIndex."
        }
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        $timeoutMs = 120000
        if (-not $process.WaitForExit($timeoutMs)) {
            Stop-BaselineProcessTree -Process $process
            throw "Timed out after $timeoutMs ms: $Backend/$Corpus/$Dpi sample $SampleIndex."
        }
        $stdout = $stdoutTask.GetAwaiter().GetResult()
        $stderr = $stderrTask.GetAwaiter().GetResult()
        if ($process.ExitCode -ne 0) {
            throw ("tests1.exe failed ({0}) for {1}/{2}/{3} sample {4}.{5}{6}" -f
                $process.ExitCode, $Backend, $Corpus, $Dpi, $SampleIndex,
                [Environment]::NewLine, ($stdout + [Environment]::NewLine + $stderr))
        }
        if (-not (Test-Path -LiteralPath $JsonPath -PathType Leaf)) {
            throw ("Child did not produce JSON: {0}{1}{2}" -f $JsonPath, [Environment]::NewLine, $stdout)
        }
        try {
            $record = Get-Content -LiteralPath $JsonPath -Raw -Encoding UTF8 | ConvertFrom-Json
        } catch {
            throw "Invalid JSON from $Backend/$Corpus/$Dpi sample ${SampleIndex}: $($_.Exception.Message)"
        }
        if ($null -eq $record -or $record.schemaVersion -ne $schemaVersion -or -not $record.available) {
            $reason = if ($null -ne $record) { $record.unavailableReason } else { 'no record' }
            throw "Unavailable/invalid baseline for $Backend/$Corpus/$Dpi sample ${SampleIndex}: $reason"
        }
        if ($record.backend -ne $Backend -or $record.corpus -ne $Corpus -or [int]$record.conditions.dpi -ne $Dpi) {
            throw "Child JSON conditions mismatch for $Backend/$Corpus/$Dpi sample $SampleIndex."
        }
        if ([int]$record.conditions.frameCount -ne $FramesPerSample -or
            @($record.timings.frameDurationMs).Count -ne $FramesPerSample) {
            throw "Frame count mismatch for $Backend/$Corpus/$Dpi sample $SampleIndex."
        }
        if ($null -eq $record.timings.PSObject.Properties['coldInitializationMs'] -or
            $null -eq $record.timings.PSObject.Properties['measurementInitializationMs'] -or
            $null -eq $record.resources.PSObject.Properties['processGlobalGdiObjectsDelta']) {
            throw "Cold/stabilization/measured timing/resource fields are missing for $Backend/$Corpus/$Dpi sample $SampleIndex."
        }
        if ([int64]$record.resources.gdiObjectsDelta -ne 0) {
            throw "Renderer-owned GDI delta is non-zero for $Backend/$Corpus/$Dpi sample ${SampleIndex}: $($record.resources.gdiObjectsDelta)."
        }
        foreach ($name in $counterNames) {
            if ($null -eq $record.counters.PSObject.Properties[$name]) {
                throw "Required counter '$name' is missing for $Backend/$Corpus/$Dpi sample $SampleIndex."
            }
        }
        if ($backend -eq 'candidate-native' -and $Corpus -eq 'ascii-shell') {
            foreach ($name in @('d2dTargetCreates', 'd2dTargetBinds', 'd2dDrawCalls', 'd2dEndDrawCalls',
                    'mapCharactersCalls', 'analysisCalls', 'glyphsCalls', 'placementCalls')) {
                if ([int64]$record.counters.$name -ne 0) {
                    throw "Candidate ASCII must not initialize or draw DirectWrite/D2D ($name=$($record.counters.$name)) for sample $SampleIndex."
                }
            }
        }
        return $record
    } finally {
        if ($null -ne $process) {
            try {
                if (-not $process.HasExited) {
                    Stop-BaselineProcessTree -Process $process
                }
            } catch {}
            $process.Dispose()
        }
    }
}

$records = [System.Collections.Generic.List[object]]::new()
foreach ($backend in $requiredBackends) {
    foreach ($corpus in $requiredCorpora) {
        foreach ($dpi in $requiredDpis) {
            for ($sample = 1; $sample -le $Samples; $sample++) {
                $jsonPath = Join-Path $runDirectory ("{0}-{1}-dpi{2}-sample{3}.json" -f $backend, $corpus, $dpi, $sample)
                $records.Add((Invoke-BaselineChild -Backend $backend -Corpus $corpus -Dpi $dpi -SampleIndex $sample -JsonPath $jsonPath))
            }
        }
    }
}

$summary = [System.Collections.Generic.List[object]]::new()
foreach ($backend in $requiredBackends) {
    foreach ($corpus in $requiredCorpora) {
        foreach ($dpi in $requiredDpis) {
            $matching = @($records | Where-Object { $_.backend -eq $backend -and $_.corpus -eq $corpus -and [int]$_.conditions.dpi -eq $dpi })
            if ($matching.Count -ne $Samples) {
                throw "Expected $Samples records for $backend/$corpus/$dpi, found $($matching.Count)."
            }
            $frames = @($matching | ForEach-Object { $_.timings.frameDurationMs } | ForEach-Object { [double]$_ })
            $cold = @($matching | ForEach-Object { [double]$_.timings.coldInitializationMs })
            $measurementInit = @($matching | ForEach-Object { [double]$_.timings.measurementInitializationMs })
            $privateDelta = @($matching | ForEach-Object { [double]$_.resources.privateBytesDelta })
            $gdiDelta = @($matching | ForEach-Object { [double]$_.resources.gdiObjectsDelta })
            $globalGdiDelta = @($matching | ForEach-Object { [double]$_.resources.processGlobalGdiObjectsDelta })
            $summary.Add([ordered]@{
                backend = $backend
                corpus = $corpus
                dpi = $dpi
                sampleCount = $matching.Count
                frameCountPerSample = $FramesPerSample
                frameMedianMs = Get-Median $frames
                frameP95Ms = Get-P95 $frames
                coldInitializationMedianMs = Get-Median $cold
                measurementInitializationMedianMs = Get-Median $measurementInit
                privateBytesDeltaMedian = Get-Median $privateDelta
                gdiObjectsDeltaMedian = Get-Median $gdiDelta
                processGlobalGdiObjectsDeltaMedian = Get-Median $globalGdiDelta
                counters = [ordered]@{
                    framesMedian = Get-Median @($matching | ForEach-Object { [double]$_.counters.frames })
                    extTextOutCallsMedian = Get-Median @($matching | ForEach-Object { [double]$_.counters.extTextOutCalls })
                    gdiBatchCountMedian = Get-Median @($matching | ForEach-Object { [double]$_.counters.gdiBatchCount })
                    drawStringCallsMedian = Get-Median @($matching | ForEach-Object { [double]$_.counters.drawStringCalls })
                    textLayoutCreatesMedian = Get-Median @($matching | ForEach-Object { [double]$_.counters.textLayoutCreates })
                    fallbackRunsMedian = Get-Median @($matching | ForEach-Object { [double]$_.counters.fallbackRuns })
                    fallbackCacheHitsMedian = Get-Median @($matching | ForEach-Object { [double]$_.counters.fallbackCacheHits })
                    fallbackCacheMissesMedian = Get-Median @($matching | ForEach-Object { [double]$_.counters.fallbackCacheMisses })
                     mapCharactersCallsMedian = Get-Median @($matching | ForEach-Object { [double]$_.counters.mapCharactersCalls })
                     analysisCallsMedian = Get-Median @($matching | ForEach-Object { [double]$_.counters.analysisCalls })
                     glyphsCallsMedian = Get-Median @($matching | ForEach-Object { [double]$_.counters.glyphsCalls })
                     placementCallsMedian = Get-Median @($matching | ForEach-Object { [double]$_.counters.placementCalls })
                    d2dTargetCreatesMedian = Get-Median @($matching | ForEach-Object { [double]$_.counters.d2dTargetCreates })
                    d2dTargetBindsMedian = Get-Median @($matching | ForEach-Object { [double]$_.counters.d2dTargetBinds })
                    d2dDrawCallsMedian = Get-Median @($matching | ForEach-Object { [double]$_.counters.d2dDrawCalls })
                    d2dEndDrawCallsMedian = Get-Median @($matching | ForEach-Object { [double]$_.counters.d2dEndDrawCalls })
                    d2dTargetLossesMedian = Get-Median @($matching | ForEach-Object { [double]$_.counters.d2dTargetLosses })
                }
            })
        }
    }
}

$jsonReport = [ordered]@{
    schemaVersion = $schemaVersion
    measurement = 'terminal-rendering-comparative-baseline'
    comparative = $true
    configuration = $Configuration
    generatedAtUtc = [DateTime]::UtcNow.ToString('o', [Globalization.CultureInfo]::InvariantCulture)
    conditions = [ordered]@{
        warmupFrames = 2
        framesPerSample = $FramesPerSample
        samples = $Samples
        dpis = $requiredDpis
        columns = 120
        rows = 8
        preferredFont = 'Cascadia Mono'
        fallbackFont = 'Consolas'
        surface = 'top-down 32-bpp DIB memory only'
    }
    backends = $requiredBackends
    corpora = $requiredCorpora
    requiredCounters = $counterNames
    summary = $summary
    records = $records
}
$jsonPath = Join-Path $outputRoot ("terminal-rendering-baseline-v{0}.json" -f $schemaVersion)
$jsonText = $jsonReport | ConvertTo-Json -Depth 12
[System.IO.File]::WriteAllText($jsonPath, $jsonText, [System.Text.UTF8Encoding]::new($false))

$markdown = @(
    '# Terminal rendering frozen baseline v1',
    '',
    ('Generated (UTC): {0}' -f $jsonReport.generatedAtUtc),
    ('Configuration: {0}; samples: {1}; frames/sample: {2}' -f $Configuration, $Samples, $FramesPerSample),
    '',
    'This report compares the frozen legacy backends with candidate-native under identical offscreen conditions. The documented 10-sample × 500-frame invocation is the authoritative comparative acceptance gate; shorter runs are smoke/diagnostic checks.',
    '',
    '## Conditions',
    '',
    '- Surface: top-down 32-bpp in-memory DIB; no screenshots or pixel dumps.',
    '- Font: Cascadia Mono preferred, Consolas fallback; 9-point-equivalent cells.',
    '- DPI: 96, 120, 144, 192; warm-up: 2 frames; each child uses one timed cold probe frame plus a two-frame untimed stabilization probe.',
    '',
    '## Summary',
    '',
    '| Backend | Corpus | DPI | Frame median (ms) | Frame p95 (ms) | Cold probe (ms) | Measured init (ms) | GDI delta | Global GDI delta |',
    '|---|---|---:|---:|---:|---:|---:|---:|---:|'
)
foreach ($item in $summary) {
    $markdown += ('| {0} | {1} | {2} | {3:N3} | {4:N3} | {5:N3} | {6:N3} | {7:N0} | {8:N0} |' -f
    $item.backend, $item.corpus, $item.dpi, $item.frameMedianMs, $item.frameP95Ms,
        $item.coldInitializationMedianMs, $item.measurementInitializationMedianMs,
        $item.gdiObjectsDeltaMedian, $item.processGlobalGdiObjectsDeltaMedian)
}
$markdown += @(
    '',
    'Counters are preserved in the versioned JSON report, including ExtTextOutW/batches, DrawString, per-run CreateTextLayout, explicit DirectWrite MapCharacters/AnalyzeScript/GetGlyphs/GetGlyphPlacements stages, and Direct2D target lifecycle fields. candidate-native maps the production plan/builtin/DWrite counters into this same schema; its ASCII path must keep all DWrite/D2D initialization and draw counters at zero.',
    '',
    ('Machine-readable report: {0}' -f $jsonPath)
)
$markdownPath = Join-Path $outputRoot ("terminal-rendering-baseline-v{0}.md" -f $schemaVersion)
[System.IO.File]::WriteAllText($markdownPath, ($markdown -join [Environment]::NewLine) + [Environment]::NewLine, [System.Text.UTF8Encoding]::new($false))

Write-Output ("JSON: {0}" -f $jsonPath)
Write-Output ("Markdown: {0}" -f $markdownPath)
