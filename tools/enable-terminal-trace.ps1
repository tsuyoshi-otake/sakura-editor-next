# Copyright (C) 2026, Sakura Editor Organization
# SPDX-License-Identifier: Zlib

[CmdletBinding()]
param(
	[Parameter(Mandatory = $true)]
	[ValidateRange(1, [int]::MaxValue)]
	[int]$TargetProcessId,

	[string]$Destination = (Join-Path ([IO.Path]::GetTempPath()) 'sakura-editor\terminal-traces')
)

$ErrorActionPreference = 'Stop'
$stopwatch = [Diagnostics.Stopwatch]::StartNew()
$target = Get-Process -Id $TargetProcessId -ErrorAction Stop
if ($target.ProcessName -ne 'sakura') {
	throw "PID $TargetProcessId is '$($target.ProcessName)', not sakura.exe."
}

$destinationPath = [IO.Path]::GetFullPath($Destination)
[IO.Directory]::CreateDirectory($destinationPath) | Out-Null
$controlDirectory = Join-Path ([IO.Path]::GetTempPath()) 'sakura-editor'
[IO.Directory]::CreateDirectory($controlDirectory) | Out-Null
$controlPath = Join-Path $controlDirectory "terminal-trace-$TargetProcessId.ini"
$controlText = "[trace]`r`ndirectory=$destinationPath`r`n"
[IO.File]::WriteAllText($controlPath, $controlText, [Text.Encoding]::Unicode)

$eventName = "Local\SakuraEditorNext.TerminalTrace.$TargetProcessId"
try {
	$activationEvent = [Threading.EventWaitHandle]::OpenExisting($eventName)
	try {
		if (-not $activationEvent.Set()) {
			throw "The trace activation event rejected the signal."
		}
	}
	finally {
		$activationEvent.Dispose()
	}
}
catch {
	[IO.File]::Delete($controlPath)
	throw "Could not activate terminal tracing for PID $TargetProcessId. Confirm that this build is running and an integrated terminal session exists. $($_.Exception.Message)"
}

$stopwatch.Stop()
[pscustomobject]@{
	ProcessId = $TargetProcessId
	Destination = $destinationPath
	EventName = $eventName
	ActivationRequested = $true
	ElapsedSeconds = [Math]::Round($stopwatch.Elapsed.TotalSeconds, 3)
}
