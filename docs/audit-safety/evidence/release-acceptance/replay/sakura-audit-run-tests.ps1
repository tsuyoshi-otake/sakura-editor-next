param([string]$Configuration='Debug',[string]$Label='focused',[string]$Filter='FileLoadOptionsTest.*:SearchRequestSafetyTest.*:SearchWorkbenchToolGeometry.*',[int]$TimeoutSeconds=120,[switch]$Unattended)
if($Unattended) { $Filter='-MacroMgrTest.*:CPpaTest.*:SelectFileTest.*:FileDialog/FileDialogTest.*:CDlgProfileMgrTest.*:TrayWndTest.*:EditWndTest.*:WinMainFuncTest.*:WinMain/WinMainTest.*' }
$ErrorActionPreference='Stop'
$taskRoot='C:/Users/developer/tmp/sakura-audit-safety'
$outRoot=Join-Path $taskRoot '.codex/goal-loop/audit-safety'
$clock=[Diagnostics.Stopwatch]::StartNew()
$exe=Join-Path $taskRoot "x64/$Configuration/tests1.exe"
$p=Start-Process -FilePath $exe -ArgumentList "--gtest_filter=$Filter","--gtest_output=xml:$outRoot/$Label.xml" -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru -RedirectStandardOutput "$outRoot/$Label.log" -RedirectStandardError "$outRoot/$Label.err"
try {
    while(!$p.WaitForExit(1000)) {
        if($clock.Elapsed.TotalSeconds -ge $TimeoutSeconds) {
            taskkill /PID $p.Id /T /F | Out-Null
            throw "Owned tests timed out after $TimeoutSeconds seconds"
        }
    }
    $code=$p.ExitCode
    Get-Content "$outRoot/$Label.log" -Tail 18
} finally {
    if(!$p.HasExited) { taskkill /PID $p.Id /T /F | Out-Null }
    $survivors=@(Get-CimInstance Win32_Process | Where-Object { $_.Name -match '^(tests1|sakura)\.exe$' -and ($_.ExecutablePath -replace '\\', '/') -like "$taskRoot/*" })
    $survivors | Select-Object ProcessId,ParentProcessId,ExecutablePath | ConvertTo-Json | Set-Content "$outRoot/$Label-processes.json"
    Write-Output "ElapsedSeconds=$($clock.Elapsed.TotalSeconds) OwnedSurvivors=$($survivors.Count)"
    if($survivors.Count -gt 0) { throw 'Owned product/test survivors require explicit parent-first cleanup' }
}
exit $code