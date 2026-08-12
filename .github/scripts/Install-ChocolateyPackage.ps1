[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateNotNullOrEmpty()]
    [string]$PackageId,

    [Parameter(Mandatory)]
    [ValidateNotNullOrEmpty()]
    [string]$Version,

    [ValidateRange(1, 10)]
    [int]$MaxAttempts = 3,

    [ValidateRange(1, 300)]
    [int]$InitialDelaySeconds = 15,

    [ValidateRange(0, 60)]
    [int]$MaxJitterSeconds = 5
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# PowerShell 7.3+ can turn a non-zero native exit code into a terminating error.
# This script decides for itself which exit codes are retryable, so opt out
# where the preference exists instead of letting it pre-empt the loop.
if (Test-Path -LiteralPath 'variable:PSNativeCommandUseErrorActionPreference') {
    $PSNativeCommandUseErrorActionPreference = $false
}

# Chocolatey reports a completed install that wants a reboot as 1641 or 3010.
# The runner is discarded when the job ends, so both are done here.
$succeeded = @(0, 1641, 3010)

$label = "$PackageId $Version"
$delaySeconds = $InitialDelaySeconds

for ($attempt = 1; $attempt -le $MaxAttempts; $attempt++) {
    Write-Host "Installing $label (attempt $attempt of $MaxAttempts)."

    & choco install $PackageId --version=$Version --yes --no-progress
    $exitCode = $LASTEXITCODE

    if ($succeeded -contains $exitCode) {
        if ($attempt -gt 1) {
            Write-Host "::notice::Installed $label on attempt $attempt after a transient Chocolatey failure."
        }

        exit 0
    }

    if ($attempt -eq $MaxAttempts) {
        throw [System.InvalidOperationException]::new(
            "choco install $label exited $exitCode on all $MaxAttempts attempts."
        )
    }

    # Report every retry. The community feed answering 503 for one job is worth
    # seeing in the run summary; a silent retry would hide a package that has
    # genuinely stopped resolving until the day it stops resolving three times.
    Write-Host "::warning::choco install $label exited $exitCode; retrying in $delaySeconds s."

    $jitterSeconds = 0
    if ($MaxJitterSeconds -gt 0) {
        $jitterSeconds = Get-Random -Minimum 0 -Maximum ($MaxJitterSeconds + 1)
    }

    Start-Sleep -Seconds ($delaySeconds + $jitterSeconds)
    $delaySeconds = $delaySeconds * 2
}
