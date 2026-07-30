[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$RepositoryRoot,
    [string]$Platform = "x64",
    [string]$Configuration = "Debug",
    [switch]$Cleanup
)

$ErrorActionPreference = "Stop"
$root = [IO.Path]::GetFullPath((Resolve-Path -LiteralPath $RepositoryRoot).Path).TrimEnd('\')
$testsPath = [IO.Path]::GetFullPath((Join-Path $root "$Platform\$Configuration\tests1.exe"))
$sakuraPath = [IO.Path]::GetFullPath((Join-Path $root "$Platform\$Configuration\sakura.exe"))
$sourceHostMarker = "$root\src\exthost"
$builtHostMarker = "$root\$Platform\$Configuration\exthost"
$acceptanceMarker = "$root\src\test\integration\open-vsx-acceptance.cjs"
$performanceMarker = "$root\src\test\integration\extension-host-performance.cjs"

foreach ($path in @($testsPath, $sakuraPath, $sourceHostMarker, $builtHostMarker, $acceptanceMarker, $performanceMarker)) {
    if (-not $path.StartsWith("$root\", [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to inspect a path outside the repository: $path"
    }
}

function Get-RepositoryTestProcesses {
    @(Get-CimInstance Win32_Process | Where-Object {
        ($_.Name -ieq "tests1.exe" -and $_.ExecutablePath -ieq $testsPath) -or
        ($_.Name -ieq "sakura.exe" -and $_.ExecutablePath -ieq $sakuraPath) -or
        ($_.Name -ieq "node.exe" -and $_.CommandLine -and (
            $_.CommandLine.IndexOf($sourceHostMarker, [StringComparison]::OrdinalIgnoreCase) -ge 0 -or
            $_.CommandLine.IndexOf($builtHostMarker, [StringComparison]::OrdinalIgnoreCase) -ge 0 -or
            $_.CommandLine.IndexOf($acceptanceMarker, [StringComparison]::OrdinalIgnoreCase) -ge 0 -or
            $_.CommandLine.IndexOf($performanceMarker, [StringComparison]::OrdinalIgnoreCase) -ge 0
        ))
    })
}

function Get-ParentFirstOrder([object[]]$Processes) {
    $remaining = @{}
    foreach ($process in $Processes) { $remaining[[int]$process.ProcessId] = $process }
    $ordered = [Collections.Generic.List[object]]::new()
    while ($remaining.Count -gt 0) {
        $roots = @($remaining.Values | Where-Object { -not $remaining.ContainsKey([int]$_.ParentProcessId) })
        if ($roots.Count -eq 0) { $roots = @($remaining.Values | Select-Object -First 1) }
        foreach ($process in $roots) {
            $ordered.Add($process)
            $remaining.Remove([int]$process.ProcessId)
        }
    }
    return $ordered
}

$targets = @(Get-RepositoryTestProcesses)
if ($targets.Count -eq 0) {
    Write-Output "PROCESS_CLEAN"
    exit 0
}

if (-not $Cleanup) {
    $targets | Select-Object ProcessId, ParentProcessId, Name, ExecutablePath, CommandLine |
        ConvertTo-Json -Compress | Write-Error
    exit 1
}

foreach ($process in (Get-ParentFirstOrder $targets)) {
    Stop-Process -Id $process.ProcessId -Force -ErrorAction SilentlyContinue
}
Start-Sleep -Milliseconds 200
$remainingTargets = @(Get-RepositoryTestProcesses)
if ($remainingTargets.Count -ne 0) {
    $remainingTargets | Select-Object ProcessId, ParentProcessId, Name, ExecutablePath, CommandLine |
        ConvertTo-Json -Compress | Write-Error
    exit 1
}

Write-Output "PROCESS_CLEANED"
