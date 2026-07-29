[CmdletBinding()]
param(
    [switch] $ValidateImportedFiles,
    [switch] $VerifyNoOpBuild,
    [switch] $ListSurvivors,
    [string] $Platform = 'x64',
    [ValidateSet('Debug', 'Release')]
    [string] $Configuration = 'Release',
    [string] $Solution = 'sakura.sln',
    [string] $TargetExe,
    [string] $BinlogDirectory = 'artifacts/build-verification',
    [string] $ProbeCpp,
    [string] $ProbeHeader,
    [string[]] $ExpectedProbeLinkConsumers,
    [ValidateRange(1, 10)]
    [int] $NoOpIterations = 3
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

function Resolve-RepositoryPath {
    param([Parameter(Mandatory)][string] $Path)
    if ([System.IO.Path]::IsPathRooted($Path)) { return (Resolve-Path -LiteralPath $Path).Path }
    return (Resolve-Path -LiteralPath (Join-Path $repoRoot $Path)).Path
}

function Get-LfNormalizedSha256 {
    param([Parameter(Mandatory)][string] $Path)
    # Git may materialize these text files as CRLF when core.autocrlf is
    # enabled.  The manifest deliberately records the repository's LF blob,
    # so normalize only CRLF pairs before hashing and preserve every other
    # byte exactly as checked out.
    $source = [System.IO.File]::ReadAllBytes($Path)
    $stream = [System.IO.MemoryStream]::new($source.Length)
    $algorithm = [System.Security.Cryptography.SHA256]::Create()
    try {
        for ($index = 0; $index -lt $source.Length; $index++) {
            if ($source[$index] -eq 0x0D -and
                $index + 1 -lt $source.Length -and
                $source[$index + 1] -eq 0x0A) {
                $index++
                $stream.WriteByte(0x0A)
            } else {
                $stream.WriteByte($source[$index])
            }
        }
        $stream.Position = 0
        return ([System.BitConverter]::ToString($algorithm.ComputeHash($stream))).Replace('-', '').ToLowerInvariant()
    } finally {
        $algorithm.Dispose()
        $stream.Dispose()
    }
}

function Get-RepositoryRelativePath {
    param([Parameter(Mandatory)][string] $Path)
    $fullPath = [System.IO.Path]::GetFullPath($Path)
    if (-not $fullPath.StartsWith($repoRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Path is outside the repository: $Path"
    }
    return $fullPath.Substring($repoRoot.Length).TrimStart([char[]]'\\/')
}

function Get-MSBuildPath {
    $candidates = [System.Collections.Generic.List[string]]::new()
    $pathCommand = Get-Command msbuild.exe -ErrorAction SilentlyContinue
    if ($pathCommand) { $candidates.Add($pathCommand.Source) }

    $vswhereCandidates = [System.Collections.Generic.List[string]]::new()
    $vswhereOnPath = Get-Command vswhere.exe -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty Source
    if ($vswhereOnPath) { $vswhereCandidates.Add($vswhereOnPath) }
    foreach ($programFiles in @($env:ProgramFiles, ${env:ProgramFiles(x86)}) | Where-Object { $_ }) {
        $vswhereCandidates.Add((Join-Path $programFiles 'Microsoft Visual Studio\Installer\vswhere.exe'))
    }
    $vswhereCandidates = @($vswhereCandidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -Unique)
    foreach ($vswhere in $vswhereCandidates) {
        $installations = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath 2>$null
        foreach ($installation in $installations) {
            foreach ($relativePath in @('MSBuild\Current\Bin\MSBuild.exe', 'MSBuild\15.0\Bin\MSBuild.exe')) {
                $candidates.Add((Join-Path $installation.Trim() $relativePath))
            }
        }
    }

    foreach ($programFiles in @($env:ProgramFiles, ${env:ProgramFiles(x86)}) | Where-Object { $_ }) {
        foreach ($version in @('2022', '2019', '2017')) {
            foreach ($edition in @('Enterprise', 'Professional', 'Community', 'BuildTools')) {
                foreach ($relativePath in @('MSBuild\Current\Bin\MSBuild.exe', 'MSBuild\15.0\Bin\MSBuild.exe')) {
                    $candidates.Add((Join-Path $programFiles "Microsoft Visual Studio\$version\$edition\$relativePath"))
                }
            }
        }
        $candidates.Add((Join-Path $programFiles 'MSBuild\Current\Bin\MSBuild.exe'))
        $candidates.Add((Join-Path $programFiles 'MSBuild\14.0\Bin\MSBuild.exe'))
    }

    foreach ($candidate in $candidates | Where-Object { $_ } | Select-Object -Unique) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { return (Resolve-Path -LiteralPath $candidate).Path }
    }
    throw 'MSBuild.exe was not found on PATH, through vswhere, or in known Visual Studio installation locations.'
}

function Get-ObjectTimestampSnapshot {
    param([Parameter(Mandatory)][string] $ObjectDirectory)
    if (-not (Test-Path -LiteralPath $ObjectDirectory -PathType Container)) {
        throw "Object directory was not found after the baseline build: $ObjectDirectory"
    }
    $snapshot = @{}
    Get-ChildItem -LiteralPath $ObjectDirectory -Recurse -Filter *.obj -File | ForEach-Object {
        $snapshot[$_.FullName] = $_.LastWriteTimeUtc.Ticks
    }
    if ($snapshot.Count -eq 0) { throw "No .obj files were found after the baseline build: $ObjectDirectory" }
    return $snapshot
}

function Assert-TimestampSnapshotUnchanged {
    param(
        [Parameter(Mandatory)][hashtable] $Before,
        [Parameter(Mandatory)][string] $ObjectDirectory,
        [Parameter(Mandatory)][string] $IterationName
    )
    $after = Get-ObjectTimestampSnapshot $ObjectDirectory
    $beforePaths = @($Before.Keys | Sort-Object)
    $afterPaths = @($after.Keys | Sort-Object)
    if (($beforePaths -join "`n") -ne ($afterPaths -join "`n")) {
        throw "$IterationName changed the set of object files under $ObjectDirectory"
    }
    $changed = @($beforePaths | Where-Object { $Before[$_] -ne $after[$_] })
    if ($changed.Count -gt 0) {
        throw "$IterationName changed .obj timestamps: $((@($changed | Select-Object -First 20)) -join ', ')"
    }
}

function Find-ExecutedCompilerTasks {
    param([Parameter(Mandatory)][string] $DiagnosticLog)
    # Match only a direct executable token with command arguments. Excluding
    # '=' from the executable path rejects properties such as
    # 'ClCompilerPath = ...\cl.exe', while the anchored token also excludes
    # Tracker.exe command echoes containing a nested compiler command.
    $invocationPattern = '(?im)^\s*(?:\d+>)?\s*"?(?:[^"=\r\n]{0,512}\\)?(?:cl|link)\.exe"?\s+(?:(?:/[^\s]+)|(?:[^\r\n]*\.(?:cpp|cxx|cc)\b))'
    return @(Select-String -LiteralPath $DiagnosticLog -Pattern $invocationPattern)
}

function Get-ExecutedBuildTaskBlocks {
    param([Parameter(Mandatory)][string] $DiagnosticLog)

    # Diagnostic MSBuild output identifies real task execution with an exact
    # `Task "CL"` / `Task "Link"` line.  Keep the task body and the most
    # recently announced vcxproj so the probe can prove both the source and
    # the linker consumer without mistaking a skipped task for execution.
    $lines = @(Get-Content -LiteralPath $DiagnosticLog)
    $blocks = [System.Collections.Generic.List[object]]::new()
    $currentProject = $null
    # The prefix is a short localized task label. Bounding it prevents a large
    # Tracker.exe command line from causing expensive regex backtracking.
    $taskStartPattern = '(?i)^\s*(?:\d+>)?\s*[^"]{0,128}"\s*(?<task>CL|Link)\s*"\s*\(TaskId:\s*(?<taskId>\d+)\)\s*$'
    $anyTaskStartPattern = '(?i)^\s*(?:\d+>)?\s*[^"]{0,128}"\s*(?<task>[^"]+)\s*"\s*\(TaskId:\s*(?<taskId>\d+)\)\s*$'
    for ($index = 0; $index -lt $lines.Count; $index++) {
        $line = $lines[$index]
        if ($line.IndexOf('.vcxproj', [System.StringComparison]::OrdinalIgnoreCase) -ge 0 -and
            $line -match '(?i)"(?<project>[^"]+\.vcxproj)"') {
            $currentProject = $Matches.project
        }
        if ($line.IndexOf('(TaskId:') -lt 0) {
            continue
        }
        if ($line -notmatch $taskStartPattern) {
            continue
        }

        $task = $Matches.task
        $end = $index
        while (($end + 1) -lt $lines.Count) {
            $end++
            if ($lines[$end].IndexOf('(TaskId:') -ge 0 -and $lines[$end] -match $anyTaskStartPattern) {
                $end--
                break
            }
        }
        $blocks.Add([pscustomobject]@{
            Task = $task
            Project = $currentProject
            StartLine = $index + 1
            Lines = @($lines[$index..$end])
        })
        $index = $end
    }
    return @($blocks)
}

function Test-BuildTaskContainsRepositoryPath {
    param(
        [Parameter(Mandatory)] $TaskBlock,
        [Parameter(Mandatory)][string] $Path
    )

    $fullPath = [System.IO.Path]::GetFullPath($Path)
    # MSBuild can print an absolute, repository-relative, or project-relative
    # source path. Requiring one complete path (rather than a filename) keeps
    # the source assertion exact while supporting all three forms.
    $candidatePaths = [System.Collections.Generic.List[string]]::new()
    $candidatePaths.Add((Get-RepositoryRelativePath $fullPath))
    $candidatePaths.Add($fullPath)
    if ($TaskBlock.Project) {
        try {
            $projectPath = Resolve-RepositoryPath $TaskBlock.Project
            $candidatePaths.Add([System.IO.Path]::GetRelativePath((Split-Path -Parent $projectPath), $fullPath))
        } catch {
            # Keep the absolute and repository-relative forms when project
            # attribution is unavailable.
        }
    }
    $pathPatterns = @($candidatePaths | Select-Object -Unique | ForEach-Object {
        [regex]::Escape($_)
    })
    $pattern = '(?i)(?<![A-Za-z0-9_.-])(?:' + ($pathPatterns -join '|') + ')(?![A-Za-z0-9_.-])'
    return [bool]($TaskBlock.Lines -match $pattern)
}

function Get-TaskBlockProjectRelativePath {
    param([Parameter(Mandatory)] $TaskBlock)
    if (-not $TaskBlock.Project) { return $null }
    try {
        return Get-RepositoryRelativePath (Resolve-RepositoryPath $TaskBlock.Project)
    } catch {
        return $null
    }
}

function Format-BuildTaskEvidence {
    param([Parameter(Mandatory)] $TaskBlock)
    $project = Get-TaskBlockProjectRelativePath $TaskBlock
    if (-not $project) { $project = '<unresolved>' }
    $header = "Task=$($TaskBlock.Task) project=$project line=$($TaskBlock.StartLine)"
    $details = @($TaskBlock.Lines | Where-Object { $_ -match '(?i)(?:cl\.exe|link\.exe|source|command line|\.cpp\b)' } | Select-Object -First 12)
    if ($details.Count -eq 0) { $details = @($TaskBlock.Lines | Select-Object -First 12) }
    return $header + "`n" + (($details | ForEach-Object { $_.Trim() }) -join "`n")
}

function Assert-ProbeIncrementalBuildEvidence {
    param(
        [Parameter(Mandatory)][string] $DiagnosticLog,
        [Parameter(Mandatory)][string] $ProbePath,
        [Parameter(Mandatory)][string[]] $ExpectedLinkConsumers
    )

    $taskBlocks = Get-ExecutedBuildTaskBlocks $DiagnosticLog
    # A CL task can run solely to determine that all inputs are up to date.
    # Count compilation only when its block logs a direct CL.exe invocation;
    # the anchored match deliberately excludes Tracker.exe command echoes.
    $compilerInvocationPattern = '(?i)^\s*(?:\d+>)?\s*"?(?:[^"=\r\n]{0,512}\\)?cl\.exe"?\s+(?:(?:/[^\s]+)|(?:[^\r\n]*\.(?:cpp|cxx|cc)\b))'
    $compilerBlocks = @($taskBlocks | Where-Object {
        $_.Task -ieq 'CL' -and @($_.Lines | Where-Object { $_ -match $compilerInvocationPattern }).Count -gt 0
    })
    $probeCompilerBlocks = @($compilerBlocks | Where-Object { Test-BuildTaskContainsRepositoryPath $_ $ProbePath })
    if ($probeCompilerBlocks.Count -ne 1) {
        $evidence = @($compilerBlocks | ForEach-Object { Format-BuildTaskEvidence $_ }) -join "`n---`n"
        throw "Probe incremental build must compile exactly one CL task for $(Get-RepositoryRelativePath $ProbePath); found $($probeCompilerBlocks.Count). Diagnostic evidence:`n$evidence"
    }

    $unexpectedCompilerBlocks = @($compilerBlocks | Where-Object { -not (Test-BuildTaskContainsRepositoryPath $_ $ProbePath) })
    if ($unexpectedCompilerBlocks.Count -gt 0) {
        $evidence = @($unexpectedCompilerBlocks | ForEach-Object { Format-BuildTaskEvidence $_ }) -join "`n---`n"
        throw "Probe incremental build compiled translation units other than $(Get-RepositoryRelativePath $ProbePath). Diagnostic evidence:`n$evidence"
    }

    $linkBlocks = @($taskBlocks | Where-Object { $_.Task -ieq 'Link' })
    $unresolvedLinkBlocks = @($linkBlocks | Where-Object { -not (Get-TaskBlockProjectRelativePath $_) })
    if ($unresolvedLinkBlocks.Count -gt 0) {
        $evidence = @($unresolvedLinkBlocks | ForEach-Object { Format-BuildTaskEvidence $_ }) -join "`n---`n"
        throw "Could not attribute executed Link task(s) to a vcxproj. Diagnostic evidence:`n$evidence"
    }
    $actualLinkConsumers = @($linkBlocks | ForEach-Object { Get-TaskBlockProjectRelativePath $_ } | Sort-Object -Unique)
    $unexpectedLinkConsumers = @($actualLinkConsumers | Where-Object { $_ -notin $ExpectedLinkConsumers })
    $missingLinkConsumers = @($ExpectedLinkConsumers | Where-Object { $_ -notin $actualLinkConsumers })
    if ($unexpectedLinkConsumers.Count -gt 0 -or $missingLinkConsumers.Count -gt 0) {
        $evidence = @($linkBlocks | ForEach-Object { Format-BuildTaskEvidence $_ }) -join "`n---`n"
        throw "Probe incremental build link consumers differed from expectation. Expected: $($ExpectedLinkConsumers -join ', '); actual: $($actualLinkConsumers -join ', '). Unexpected: $($unexpectedLinkConsumers -join ', '); missing: $($missingLinkConsumers -join ', '). Diagnostic evidence:`n$evidence"
    }
    Write-Host "Probe incremental build passed: compiled $(Get-RepositoryRelativePath $ProbePath) exactly once; link consumers: $($actualLinkConsumers -join ', ')."
}

function Get-HeaderProbeIncrementalBuildEvidence {
    param([Parameter(Mandatory)][string] $DiagnosticLog)

    $taskBlocks = Get-ExecutedBuildTaskBlocks $DiagnosticLog
    $compilerInvocationPattern = '(?i)^\s*(?:\d+>)?\s*"?(?:[^"=\r\n]{0,512}\\)?cl\.exe"?\s+(?:(?:/[^\s]+)|(?:[^\r\n]*\.(?:cpp|cxx|cc)\b))'
    $compilerBlocks = @($taskBlocks | Where-Object {
        $_.Task -ieq 'CL' -and @($_.Lines | Where-Object { $_ -match $compilerInvocationPattern }).Count -gt 0
    })
    $sourcePattern = '(?i)(?:"(?<quoted>[^"\r\n]+\.(?:cpp|cxx|cc))"|(?<bare>[^\s"\r\n]+\.(?:cpp|cxx|cc)))'
    $translationUnits = [System.Collections.Generic.List[string]]::new()
    foreach ($block in $compilerBlocks) {
        foreach ($line in $block.Lines) {
            foreach ($match in [regex]::Matches($line, $sourcePattern)) {
                $source = if ($match.Groups['quoted'].Success) { $match.Groups['quoted'].Value } else { $match.Groups['bare'].Value }
                if ($source) { $translationUnits.Add($source) }
            }
        }
    }
    $linkConsumers = @($taskBlocks | Where-Object { $_.Task -ieq 'Link' } | ForEach-Object {
        Get-TaskBlockProjectRelativePath $_
    } | Where-Object { $_ } | Sort-Object -Unique)
    return [pscustomobject]@{
        compiledTranslationUnitCount = $compilerBlocks.Count
        compiledTranslationUnits = @($translationUnits | Sort-Object -Unique)
        linkConsumers = $linkConsumers
    }
}

function Test-WindowsTerminalImportedFiles {
    $manifest = Join-Path $repoRoot 'sakura_core/terminal/vendor/windows_terminal/IMPORTED_FILES.md'
    if (-not (Test-Path -LiteralPath $manifest -PathType Leaf)) { throw "Windows Terminal import manifest not found: $manifest" }
    $entries = @(Get-Content -LiteralPath $manifest | ForEach-Object {
        if ($_ -match '^\|\s*`(?<upstream>[^`]+)`\s*\|\s*`(?<path>[^`]+)`\s*\|\s*`(?<upstreamHash>[0-9a-fA-F]{64})`\s*\|\s*`(?<vendoredHash>[0-9a-fA-F]{64})`\s*\|\s*$') {
            [pscustomobject]@{ Path = $Matches.path; ExpectedHash = $Matches.vendoredHash.ToLowerInvariant() }
        }
    })
    if ($entries.Count -eq 0) { throw "No SHA-256 entries were parsed from $manifest" }

    $failures = [System.Collections.Generic.List[string]]::new()
    foreach ($entry in $entries) {
        $file = Join-Path $repoRoot $entry.Path
        if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { $failures.Add("missing: $($entry.Path)"); continue }
        $actualHash = Get-LfNormalizedSha256 $file
        if ($actualHash -ne $entry.ExpectedHash) { $failures.Add("SHA-256 mismatch: $($entry.Path) expected=$($entry.ExpectedHash) actual=$actualHash") }
    }
    if ($failures.Count -gt 0) { throw ("Windows Terminal imported-file verification failed:`n" + ($failures -join "`n")) }
    Write-Host "Windows Terminal imported files verified: $($entries.Count) SHA-256 entries."
}

function Get-CompiledTuConsumers {
    param([Parameter(Mandatory)][string] $CppPath)
    $consumers = foreach ($project in Get-ChildItem -LiteralPath $repoRoot -Recurse -Filter *.vcxproj -File) {
        [xml] $xml = Get-Content -LiteralPath $project.FullName -Raw
        $namespace = [System.Xml.XmlNamespaceManager]::new($xml.NameTable)
        $namespace.AddNamespace('msb', 'http://schemas.microsoft.com/developer/msbuild/2003')
        $includes = $xml.SelectNodes('//msb:ClCompile[@Include]', $namespace) | ForEach-Object { $_.GetAttribute('Include').Replace('/', '\\') }
        $matchesProbe = foreach ($include in $includes) {
            try {
                if ([System.IO.Path]::GetFullPath((Join-Path $project.DirectoryName $include)) -ieq $CppPath) {
                    $true
                }
            } catch {
                # An MSBuild expression cannot refer to a concrete probe path.
                continue
            }
        }
        if ($matchesProbe) {
            Get-RepositoryRelativePath $project.FullName
        }
    }
    return @($consumers)
}

function Invoke-NoOpBuildVerification {
    $msbuild = Get-MSBuildPath
    $solutionPath = Resolve-RepositoryPath $Solution
    $exeRelativePath = if ($TargetExe) { $TargetExe } else { "$Platform\\$Configuration\\sakura.exe" }
    # The target may not exist until the baseline build completes, so do not
    # resolve it through the filesystem yet.
    $exePath = if ([System.IO.Path]::IsPathRooted($exeRelativePath)) {
        [System.IO.Path]::GetFullPath($exeRelativePath)
    } else {
        [System.IO.Path]::GetFullPath((Join-Path $repoRoot $exeRelativePath))
    }
    $objectDirectory = Join-Path $repoRoot "build\\$Platform\\$Configuration"
    $logDirectory = Join-Path $repoRoot $BinlogDirectory
    New-Item -ItemType Directory -Path $logDirectory -Force | Out-Null

    if ($ProbeCpp -and $ProbeHeader) { throw 'Specify either ProbeCpp or ProbeHeader, not both.' }
    $probePath = $null
    $headerProbePath = $null
    $originalProbeTime = $null
    $originalHeaderProbeTime = $null
    # PowerShell variable names are case-insensitive. Preserve the public
    # parameter under a distinct name before building the resolved list.
    $requestedProbeLinkConsumers = @($ExpectedProbeLinkConsumers)
    $resolvedProbeLinkConsumers = @()
    if ($ProbeCpp) {
        $probePath = Resolve-RepositoryPath $ProbeCpp
        if ([System.IO.Path]::GetExtension($probePath) -ine '.cpp') { throw "ProbeCpp must name a .cpp file: $ProbeCpp" }
        $consumers = Get-CompiledTuConsumers $probePath
        Write-Host "Probe translation unit: $(Get-RepositoryRelativePath $probePath)"
        if ($requestedProbeLinkConsumers) {
            foreach ($consumer in $requestedProbeLinkConsumers) {
                $consumerPath = Resolve-RepositoryPath $consumer
                if ([System.IO.Path]::GetExtension($consumerPath) -ine '.vcxproj') {
                    throw "ExpectedProbeLinkConsumers must name .vcxproj files: $consumer"
                }
                $resolvedProbeLinkConsumers += Get-RepositoryRelativePath $consumerPath
            }
        } else {
            $resolvedProbeLinkConsumers = @($consumers)
        }
        $resolvedProbeLinkConsumers = @($resolvedProbeLinkConsumers | Sort-Object -Unique)
        if ($resolvedProbeLinkConsumers.Count -eq 0) {
            throw 'No expected .vcxproj Link consumers were found for the probe translation unit. Specify -ExpectedProbeLinkConsumers explicitly if the source is consumed indirectly.'
        }
        Write-Host ('Expected probe Link consumers: ' + ($resolvedProbeLinkConsumers -join ', '))
        $originalProbeTime = (Get-Item -LiteralPath $probePath).LastWriteTimeUtc
    }
    elseif ($ProbeHeader) {
        $headerProbePath = Resolve-RepositoryPath $ProbeHeader
        if ([System.IO.Path]::GetExtension($headerProbePath) -notin @('.h', '.hpp', '.hxx')) {
            throw "ProbeHeader must name a C/C++ header: $ProbeHeader"
        }
        $originalHeaderProbeTime = (Get-Item -LiteralPath $headerProbePath).LastWriteTimeUtc
        Write-Host "Header probe: $(Get-RepositoryRelativePath $headerProbePath)"
    }

    try {
        $baselineBinlog = Join-Path $logDirectory "$Platform-$Configuration-baseline.binlog"
        Remove-Item -LiteralPath $baselineBinlog -Force -ErrorAction SilentlyContinue
        $probeBinlog = Join-Path $logDirectory "$Platform-$Configuration-probe.binlog"
        $probeDiagnosticLog = Join-Path $logDirectory "$Platform-$Configuration-probe.diagnostic.log"
        Remove-Item -LiteralPath $probeBinlog, $probeDiagnosticLog -Force -ErrorAction SilentlyContinue
        for ($iteration = 1; $iteration -le $NoOpIterations; $iteration++) {
            $iterationTag = '{0:D2}' -f $iteration
            Remove-Item -LiteralPath (Join-Path $logDirectory "$Platform-$Configuration-noop-$iterationTag.binlog"), (Join-Path $logDirectory "$Platform-$Configuration-noop-$iterationTag.diagnostic.log") -Force -ErrorAction SilentlyContinue
        }

        Write-Host "Using MSBuild: $msbuild"
        Write-Host "Running baseline build: $solutionPath ($Platform $Configuration)"
        & $msbuild $solutionPath "/p:Platform=$Platform" "/p:Configuration=$Configuration" "/bl:$baselineBinlog" /nologo
        if ($LASTEXITCODE -ne 0) { throw "Baseline MSBuild failed with exit code $LASTEXITCODE" }
        if (-not (Test-Path -LiteralPath $exePath -PathType Leaf)) { throw "Expected target executable was not produced: $exePath" }
        $baselineExeTimestamp = (Get-Item -LiteralPath $exePath).LastWriteTimeUtc
        $baselineObjectTimestamps = Get-ObjectTimestampSnapshot $objectDirectory

        if ($probePath) {
            # The source is deliberately touched only after the baseline has
            # finished.  This prevents the baseline from absorbing the change
            # and turning the probe into a sequence of no-op builds.
            $probeTouchTime = [DateTime]::UtcNow.AddSeconds(2)
            if ($probeTouchTime -le $originalProbeTime) { $probeTouchTime = $originalProbeTime.AddSeconds(2) }
            (Get-Item -LiteralPath $probePath).LastWriteTimeUtc = $probeTouchTime
            Write-Host "Running probe incremental build after touching $(Get-RepositoryRelativePath $probePath): $solutionPath ($Platform $Configuration)"
            & $msbuild $solutionPath "/p:Platform=$Platform" "/p:Configuration=$Configuration" "/bl:$probeBinlog" "/flp:logfile=$probeDiagnosticLog;verbosity=diagnostic" /verbosity:diagnostic /nologo
            if ($LASTEXITCODE -ne 0) { throw "Probe incremental MSBuild failed with exit code $LASTEXITCODE" }
            Assert-ProbeIncrementalBuildEvidence -DiagnosticLog $probeDiagnosticLog -ProbePath $probePath -ExpectedLinkConsumers $resolvedProbeLinkConsumers

            # A successful probe legitimately changes its object and linked
            # executable.  The following no-op checks must snapshot that now
            # stable post-probe state, not the pre-touch baseline.
            $baselineExeTimestamp = (Get-Item -LiteralPath $exePath).LastWriteTimeUtc
            $baselineObjectTimestamps = Get-ObjectTimestampSnapshot $objectDirectory
        }
        elseif ($headerProbePath) {
            # Header changes intentionally may rebuild more than one translation
            # unit.  Record the exact compiler/link impact rather than applying
            # the private-.cpp one-TU assertion used by ProbeCpp.
            $headerTouchTime = [DateTime]::UtcNow.AddSeconds(2)
            if ($headerTouchTime -le $originalHeaderProbeTime) { $headerTouchTime = $originalHeaderProbeTime.AddSeconds(2) }
            (Get-Item -LiteralPath $headerProbePath).LastWriteTimeUtc = $headerTouchTime
            $headerProbeBinlog = Join-Path $logDirectory "$Platform-$Configuration-header-probe.binlog"
            $headerProbeDiagnosticLog = Join-Path $logDirectory "$Platform-$Configuration-header-probe.diagnostic.log"
            Remove-Item -LiteralPath $headerProbeBinlog, $headerProbeDiagnosticLog -Force -ErrorAction SilentlyContinue
            Write-Host "Running header incremental probe after touching $(Get-RepositoryRelativePath $headerProbePath): $solutionPath ($Platform $Configuration)"
            & $msbuild $solutionPath "/p:Platform=$Platform" "/p:Configuration=$Configuration" "/bl:$headerProbeBinlog" "/flp:logfile=$headerProbeDiagnosticLog;verbosity=diagnostic" /verbosity:diagnostic /nologo
            if ($LASTEXITCODE -ne 0) { throw "Header probe incremental MSBuild failed with exit code $LASTEXITCODE" }
            $headerEvidence = Get-HeaderProbeIncrementalBuildEvidence -DiagnosticLog $headerProbeDiagnosticLog
            Write-Host ('Header probe evidence: ' + ($headerEvidence | ConvertTo-Json -Depth 4 -Compress))

            # As with the .cpp probe, start no-op checks from the stable state
            # after the deliberate header touch and its expected rebuild scope.
            $baselineExeTimestamp = (Get-Item -LiteralPath $exePath).LastWriteTimeUtc
            $baselineObjectTimestamps = Get-ObjectTimestampSnapshot $objectDirectory
        }

        for ($iteration = 1; $iteration -le $NoOpIterations; $iteration++) {
            $iterationTag = '{0:D2}' -f $iteration
            $iterationName = "No-op build $iteration of $NoOpIterations"
            $binlog = Join-Path $logDirectory "$Platform-$Configuration-noop-$iterationTag.binlog"
            $diagnosticLog = Join-Path $logDirectory "$Platform-$Configuration-noop-$iterationTag.diagnostic.log"
            Write-Host "Running ${iterationName}: $solutionPath ($Platform $Configuration)"
            & $msbuild $solutionPath "/p:Platform=$Platform" "/p:Configuration=$Configuration" "/bl:$binlog" "/flp:logfile=$diagnosticLog;verbosity=diagnostic" /verbosity:diagnostic /nologo
            if ($LASTEXITCODE -ne 0) { throw "$iterationName MSBuild failed with exit code $LASTEXITCODE" }
            $currentExeTimestamp = (Get-Item -LiteralPath $exePath).LastWriteTimeUtc
            if ($baselineExeTimestamp -ne $currentExeTimestamp) {
                throw "$iterationName changed $exeRelativePath timestamp: baseline=$baselineExeTimestamp current=$currentExeTimestamp"
            }
            Assert-TimestampSnapshotUnchanged -Before $baselineObjectTimestamps -ObjectDirectory $objectDirectory -IterationName $iterationName
            $compilerTasks = Find-ExecutedCompilerTasks $diagnosticLog
            if ($compilerTasks) {
                $evidence = ($compilerTasks | Select-Object -First 20 | ForEach-Object { $_.Line.Trim() }) -join "`n"
                throw "$iterationName executed compiler/linker work:`n$evidence"
            }
            Write-Host "$iterationName passed: executable and .obj timestamps unchanged; no compiler/linker execution logged."
        }
        Write-Host "No-op verification passed for $NoOpIterations consecutive iterations. Binlogs: $logDirectory"
    } finally {
        if ($null -ne $originalProbeTime) { (Get-Item -LiteralPath $probePath).LastWriteTimeUtc = $originalProbeTime; Write-Host 'Restored ProbeCpp LastWriteTimeUtc.' }
        if ($null -ne $originalHeaderProbeTime) { (Get-Item -LiteralPath $headerProbePath).LastWriteTimeUtc = $originalHeaderProbeTime; Write-Host 'Restored ProbeHeader LastWriteTimeUtc.' }
    }
}

function Show-RepositoryLinkedSurvivors {
    $repoPattern = [regex]::Escape($repoRoot)
    # Include the complete terminal/ConPTY process shape in a report while
    # retaining repository path attribution.  This command is intentionally
    # report-only: benchmark cleanup uses its own PID + creation-time evidence.
    $namePattern = '^(sakura|tests1|msbuild|cl|link|tracker|pwsh|powershell|cmd|openconsole|conhost|cmake|ninja|python)(\.exe)?$'
    # Exclude this PowerShell/RTK ancestry.  Its command line necessarily names
    # the repository, but it is the verifier itself rather than a survivor.
    $callerAndParents = [System.Collections.Generic.HashSet[int]]::new()
    $processId = $PID
    for ($depth = 0; $depth -lt 16 -and $processId -gt 0; $depth++) {
        [void]$callerAndParents.Add([int]$processId)
        $current = Get-CimInstance Win32_Process -Filter "ProcessId = $processId" -ErrorAction SilentlyContinue
        if (-not $current) { break }
        $processId = [int]$current.ParentProcessId
    }
    $survivors = @(Get-CimInstance Win32_Process | Where-Object {
        -not $callerAndParents.Contains([int]$_.ProcessId) -and
        $_.Name -match $namePattern -and (($_.ExecutablePath -match $repoPattern) -or ($_.CommandLine -match $repoPattern))
    } | Select-Object ProcessId, ParentProcessId, Name, ExecutablePath, CommandLine)
    if ($survivors.Count -eq 0) { Write-Host 'No repository-linked build or test process survivors found.'; return }
    Write-Warning "Repository-linked build or test process survivors found (reported only; nothing is terminated): $($survivors.Count)"
    $survivors | Format-Table -AutoSize | Out-String | Write-Host
}

if (-not ($ValidateImportedFiles -or $VerifyNoOpBuild -or $ListSurvivors)) { $ValidateImportedFiles = $true }
if ($ValidateImportedFiles) { Test-WindowsTerminalImportedFiles }
if ($VerifyNoOpBuild) { Invoke-NoOpBuildVerification }
if ($ListSurvivors) { Show-RepositoryLinkedSurvivors }
