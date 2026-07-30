[CmdletBinding()]
param(
    [string] $RepositoryRoot,
    [ValidateRange(1, 60)]
    [int] $WaitSeconds = 10
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($RepositoryRoot)) {
    $RepositoryRoot = Join-Path $PSScriptRoot '..'
}
$repoRoot = (Resolve-Path -LiteralPath $RepositoryRoot).Path.TrimEnd('\', '/')
if ([string]::IsNullOrWhiteSpace($repoRoot) -or $repoRoot.Length -lt 4) {
    throw "Refusing to inspect processes for an unsafe repository root: '$repoRoot'."
}

$repoPrefixes = @(
    "$repoRoot\",
    (($repoRoot -replace '\\', '/') + '/')
)
$testProcessNamePattern = '^(OpenCppCoverage|tests1|sakura)\.exe$'

function Test-RepositoryLinkedProcess {
    param([Parameter(Mandatory)] $Process)

    if ($Process.Name -notmatch $testProcessNamePattern) {
        return $false
    }

    $executablePath = [string] $Process.ExecutablePath
    $commandLine = [string] $Process.CommandLine
    foreach ($prefix in $repoPrefixes) {
        if ($executablePath.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase) -or
            $commandLine.IndexOf($prefix, [System.StringComparison]::OrdinalIgnoreCase) -ge 0) {
            return $true
        }
    }
    return $false
}

function Get-RepositoryTestProcesses {
    return @(Get-CimInstance Win32_Process -ErrorAction Stop | Where-Object {
        Test-RepositoryLinkedProcess $_
    })
}

function Get-TargetDepth {
    param(
        [Parameter(Mandatory)] $Process,
        [Parameter(Mandatory)] [hashtable] $ProcessById
    )

    $depth = 0
    $parentId = [int] $Process.ParentProcessId
    $visited = [System.Collections.Generic.HashSet[int]]::new()
    while ($parentId -gt 0 -and $ProcessById.ContainsKey($parentId) -and $visited.Add($parentId)) {
        $depth++
        $parentId = [int] $ProcessById[$parentId].ParentProcessId
    }
    return $depth
}

$survivors = @(Get-RepositoryTestProcesses)
if ($survivors.Count -eq 0) {
    Write-Host 'No repository-linked test process survivors found.'
    exit 0
}

Write-Warning "Repository-linked test process survivors found: $($survivors.Count)"
$survivors |
    Select-Object ProcessId, ParentProcessId, Name, ExecutablePath, CommandLine |
    Format-Table -AutoSize |
    Out-String |
    Write-Host

# Stop owners before their children so a live test runner cannot respawn an
# editor process while cleanup is in progress.
$processById = @{}
foreach ($process in $survivors) {
    $processById[[int] $process.ProcessId] = $process
}
$orderedSurvivors = @($survivors | ForEach-Object {
    [pscustomobject]@{
        Depth = Get-TargetDepth -Process $_ -ProcessById $processById
        Process = $_
    }
} | Sort-Object Depth, @{ Expression = { [int] $_.Process.ProcessId } })

foreach ($entry in $orderedSurvivors) {
    $processId = [int] $entry.Process.ProcessId
    $current = Get-CimInstance Win32_Process -Filter "ProcessId = $processId" -ErrorAction SilentlyContinue
    if (-not $current -or -not (Test-RepositoryLinkedProcess $current)) {
        continue
    }

    Write-Warning "Stopping $($current.Name) (PID $processId, parent $($current.ParentProcessId))."
    Stop-Process -Id $processId -Force -ErrorAction Stop
}

$deadline = [DateTime]::UtcNow.AddSeconds($WaitSeconds)
do {
    $remaining = @(Get-RepositoryTestProcesses)
    if ($remaining.Count -eq 0) {
        break
    }
    Start-Sleep -Milliseconds 100
} while ([DateTime]::UtcNow -lt $deadline)

if ($remaining.Count -ne 0) {
    $details = $remaining |
        Select-Object ProcessId, ParentProcessId, Name, ExecutablePath, CommandLine |
        Format-Table -AutoSize |
        Out-String
    throw "Unable to terminate repository-linked test processes within $WaitSeconds seconds:`n$details"
}

throw "The test suite leaked $($survivors.Count) repository-linked process(es); cleanup terminated them."
