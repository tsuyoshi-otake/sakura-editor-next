[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$LiteralPath,

    [ValidateRange(1, 20)]
    [int]$MaxAttempts = 5,

    [ValidateRange(0, 60000)]
    [int]$InitialDelayMilliseconds = 50,

    [ValidateRange(0, 1000)]
    [int]$MaxJitterMilliseconds = 25
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$isEmptyPath = [string]::IsNullOrWhiteSpace($LiteralPath)
if ($isEmptyPath) {
    throw [System.ArgumentException]::new('LiteralPath must name one file.', 'LiteralPath')
}

try {
    $fullPath = [System.IO.Path]::GetFullPath($LiteralPath)
}
catch {
    throw [System.ArgumentException]::new("LiteralPath is invalid: '$LiteralPath'.", 'LiteralPath', $_.Exception)
}

$hasTrailingSeparator = $LiteralPath.EndsWith([System.IO.Path]::DirectorySeparatorChar) -or
    $LiteralPath.EndsWith([System.IO.Path]::AltDirectorySeparatorChar)
$pathRoot = [System.IO.Path]::GetPathRoot($fullPath)
$isFileSystemRoot = [string]::Equals(
    $fullPath.TrimEnd([char[]]'\/'),
    $pathRoot.TrimEnd([char[]]'\/'),
    [System.StringComparison]::OrdinalIgnoreCase
)
$isExistingDirectory = Test-Path -LiteralPath $fullPath -PathType Container -ErrorAction Stop
if ($hasTrailingSeparator -or $isFileSystemRoot -or $isExistingDirectory) {
    throw [System.ArgumentException]::new("LiteralPath must name one file, not a directory or root: '$LiteralPath'.", 'LiteralPath')
}

$LiteralPath = $fullPath

for ($attempt = 1; $attempt -le $MaxAttempts; $attempt++) {
    if (-not (Test-Path -LiteralPath $LiteralPath -PathType Leaf -ErrorAction Stop)) {
        return
    }

    try {
        $item = Get-Item -LiteralPath $LiteralPath -Force -ErrorAction Stop
        if ($item.IsReadOnly) {
            $item.IsReadOnly = $false
        }

        Remove-Item -LiteralPath $LiteralPath -Force -ErrorAction Stop
        if (-not (Test-Path -LiteralPath $LiteralPath -PathType Leaf -ErrorAction Stop)) {
            return
        }

        throw [System.IO.IOException]::new('The file still exists after removal.')
    }
    catch {
        if ($attempt -eq $MaxAttempts) {
            throw [System.IO.IOException]::new(
                "Failed to remove '$LiteralPath' after $MaxAttempts attempts. Last error: $($_.Exception.Message)",
                $_.Exception
            )
        }

        $exponentialDelay = $InitialDelayMilliseconds * [Math]::Pow(2, $attempt - 1)
        $jitter = if ($MaxJitterMilliseconds -gt 0) {
            Get-Random -Minimum 0 -Maximum ($MaxJitterMilliseconds + 1)
        }
        else {
            0
        }
        Start-Sleep -Milliseconds ([int]($exponentialDelay + $jitter))
    }
}

throw [System.InvalidOperationException]::new('The bounded removal loop ended without a terminal result.')
