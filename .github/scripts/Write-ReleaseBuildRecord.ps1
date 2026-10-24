[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidatePattern('^[0-9a-fA-F]{40}$')]
    [string] $SourceSha,

    [Parameter(Mandatory)]
    [ValidatePattern('^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$')]
    [string] $Repository,

    [Parameter(Mandatory)]
    [ValidateRange(1, 9223372036854775807)]
    [Int64] $RunId,

    [Parameter(Mandatory)]
    [ValidateRange(1, 9223372036854775807)]
    [Int64] $RunAttempt,

    [Parameter(Mandatory)] [string] $EventName,
    [Parameter(Mandatory)] [string] $Ref,
    [Parameter(Mandatory)] [string] $Platform,
    [Parameter(Mandatory)] [string] $Configuration,
    [Parameter(Mandatory)] [string] $Utf16Backend,
    [Parameter(Mandatory)] [string] $OutputBackend,
    [Parameter(Mandatory)] [string] $Utf16ProductionPackage,
    [Parameter(Mandatory)] [string] $OutputProductionPackage,
    [Parameter(Mandatory)]
    [string] $OutputPath
)

# Records what one main-push Release job built and tested, at the place it was
# built: the exact source, the run and attempt that produced it, the build
# contract it was given, and the hash of every packaged file. Release promotion
# reuses those bytes only when this record matches them (#376). The record
# states facts; Import-ReleaseBuildRecord.ps1 owns the release policy.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Invoke-Git {
    param(
        [Parameter(Mandatory)]
        [string[]] $Arguments
    )

    $result = & git @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "git $($Arguments -join ' ') failed with exit code $LASTEXITCODE."
    }
    return ($result | Out-String).Trim()
}

function Find-SingleFile {
    param(
        [Parameter(Mandatory)] [string] $Directory,
        [Parameter(Mandatory)] [string] $Pattern,
        [Parameter(Mandatory)] [string] $Description
    )

    $fullDirectory = [IO.Path]::GetFullPath($Directory)
    if (-not [IO.Directory]::Exists($fullDirectory)) {
        throw "$Description directory does not exist: $fullDirectory"
    }

    $matches = @([IO.Directory]::EnumerateFiles(
        $fullDirectory,
        $Pattern,
        [IO.SearchOption]::TopDirectoryOnly
    ))
    if ($matches.Count -ne 1) {
        throw "Expected exactly one $Description matching '$Pattern' in $fullDirectory; found $($matches.Count)."
    }
    return $matches[0]
}

function Get-Sha256 {
    param([Parameter(Mandatory)] [string] $Path)

    $stream = [IO.File]::OpenRead($Path)
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try {
        return ([BitConverter]::ToString($algorithm.ComputeHash($stream))).Replace('-', '').ToLowerInvariant()
    }
    finally {
        $algorithm.Dispose()
        $stream.Dispose()
    }
}

function Get-FileRecord {
    param(
        [Parameter(Mandatory)] [string] $Path,
        [Parameter(Mandatory)] [string] $Role
    )

    $file = [IO.FileInfo]::new($Path)
    if (-not $file.Exists) {
        throw "Required $Role file does not exist: $Path"
    }

    return [ordered]@{
        role = $Role
        name = $file.Name
        sha256 = Get-Sha256 -Path $file.FullName
        size_bytes = [int64] $file.Length
    }
}

function Add-BinaryVersion {
    param(
        [Parameter(Mandatory)] [System.Collections.IDictionary] $Record,
        [Parameter(Mandatory)] [string] $Path
    )

    $version = [Diagnostics.FileVersionInfo]::GetVersionInfo($Path)
    if ([string]::IsNullOrWhiteSpace($version.FileVersion) -or
        [string]::IsNullOrWhiteSpace($version.ProductVersion)) {
        throw "$($Record.role) does not expose both FileVersion and ProductVersion: $Path"
    }
    $Record.file_version = $version.FileVersion.Trim()
    $Record.product_version = $version.ProductVersion.Trim()
}

function Write-Utf8NoBomFile {
    param(
        [Parameter(Mandatory)] [string] $Path,
        [Parameter(Mandatory)] [string] $Content
    )

    $fullPath = [IO.Path]::GetFullPath($Path)
    $directory = Split-Path -Parent $fullPath
    if (-not [string]::IsNullOrWhiteSpace($directory)) {
        [IO.Directory]::CreateDirectory($directory) | Out-Null
    }
    [IO.File]::WriteAllText($fullPath, $Content, [Text.UTF8Encoding]::new($false))
}

$sourceSha = $SourceSha.ToLowerInvariant()
$facts = [ordered]@{
    event = $EventName
    ref = $Ref
    platform = $Platform
    configuration = $Configuration
    utf16_backend = $Utf16Backend
    output_backend = $OutputBackend
    utf16_production_package = $Utf16ProductionPackage
    output_production_package = $OutputProductionPackage
}
foreach ($entry in $facts.GetEnumerator()) {
    if ([string]::IsNullOrWhiteSpace($entry.Value) -or $entry.Value -cne $entry.Value.Trim()) {
        throw "Build record value '$($entry.Key)' must be a non-empty string without surrounding whitespace."
    }
}

$headSha = (Invoke-Git -Arguments @('rev-parse', 'HEAD')).ToLowerInvariant()
if ($headSha -ne $sourceSha) {
    throw "Build checkout is $headSha, expected the recorded source SHA $sourceSha."
}

$application = Get-FileRecord -Path ([IO.Path]::GetFullPath('x64/Release/sakura.exe')) -Role 'application'
Add-BinaryVersion -Record $application -Path ([IO.Path]::GetFullPath('x64/Release/sakura.exe'))
$installerPath = Find-SingleFile -Directory 'installer/Output-x64' -Pattern 'sakura_install*.exe' -Description 'x64 installer'
$installer = Get-FileRecord -Path $installerPath -Role 'installer'
Add-BinaryVersion -Record $installer -Path $installerPath

$archiveDefinitions = @(
    [pscustomobject]@{ role = 'installer_archive'; pattern = 'sakura-*-Installer.zip' },
    [pscustomobject]@{ role = 'executable_archive'; pattern = 'sakura-*-Exe.zip' },
    [pscustomobject]@{ role = 'log_archive'; pattern = 'sakura-*-Log.zip' },
    [pscustomobject]@{ role = 'assembly_archive'; pattern = 'sakura-*-Asm.zip' },
    [pscustomobject]@{ role = 'development_archive'; pattern = 'sakura-*-Dev.zip' }
)
$archives = foreach ($definition in $archiveDefinitions) {
    $archivePath = Find-SingleFile -Directory '.' -Pattern $definition.pattern -Description $definition.role
    Get-FileRecord -Path $archivePath -Role $definition.role
}

$result = [ordered]@{
    schema = 1
    source = [ordered]@{
        sha = $sourceSha
    }
    run = [ordered]@{
        repository = $Repository
        id = $RunId
        attempt = $RunAttempt
        event = $EventName
        ref = $Ref
    }
    build_contract = [ordered]@{
        platform = $Platform
        configuration = $Configuration
        utf16_backend = $Utf16Backend
        output_backend = $OutputBackend
        utf16_production_package = $Utf16ProductionPackage
        output_production_package = $OutputProductionPackage
    }
    payload = [ordered]@{
        application = $application
        installer = $installer
        archives = @($archives)
    }
}

Write-Utf8NoBomFile -Path $OutputPath -Content ($result | ConvertTo-Json -Depth 8)
Write-Host "Recorded the tested $Configuration build of $sourceSha from run $RunId attempt $RunAttempt."
