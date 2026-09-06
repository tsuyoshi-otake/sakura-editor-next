$ErrorActionPreference='Stop'
$root='C:\Users\developer\tmp\sakura-audit-safety\x64\Release\'
$names=@('tests1-audit-perf-fixed.exe','tests1-audit-perf-borrowed.exe')
$remaining=@(Get-CimInstance Win32_Process | Where-Object { $names -contains $_.Name -and $_.ExecutablePath.StartsWith($root,[StringComparison]::OrdinalIgnoreCase) })
if($remaining.Count) {
    $remaining | Select-Object ProcessId,ParentProcessId,ExecutablePath | ConvertTo-Json
    foreach($process in $remaining) {taskkill /PID $process.ProcessId /T /F | Out-Null}
    $again=@(Get-CimInstance Win32_Process | Where-Object {$names -contains $_.Name -and $_.ExecutablePath.StartsWith($root,[StringComparison]::OrdinalIgnoreCase)})
    if($again.Count) {throw 'Owned benchmark process survived cleanup'}
    throw 'Owned benchmark process required forced cleanup'
}
'OwnedBenchmarkSurvivors=0'
& 'C:/Users/developer/tmp/sakura-audit-safety/tools/Cleanup-TestProcessSurvivors.ps1' -RepositoryRoot 'C:/Users/developer/tmp/sakura-audit-safety' -WaitSeconds 5
