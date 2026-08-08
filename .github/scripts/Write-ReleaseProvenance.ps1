[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidatePattern('^[0-9a-fA-F]{40}$')]
    [string] $SourceSha,

    [Parameter(Mandatory)]
    [ValidatePattern('^v(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)-build\.(?:[1-9][0-9]*)$')]
    [string] $ReleaseTag,

    [Parameter(Mandatory)]
    [string] $OutputPath
)

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

function Get-BinaryVersion {
    param(
        [Parameter(Mandatory)] [string] $Path,
        [Parameter(Mandatory)] [string] $Description
    )

    $version = [Diagnostics.FileVersionInfo]::GetVersionInfo($Path)
    if ([string]::IsNullOrWhiteSpace($version.FileVersion) -or
        [string]::IsNullOrWhiteSpace($version.ProductVersion)) {
        throw "$Description does not expose both FileVersion and ProductVersion: $Path"
    }

    return [ordered]@{
        file_version = $version.FileVersion.Trim()
        product_version = $version.ProductVersion.Trim()
    }
}

function Assert-BinaryVersion {
    param(
        [Parameter(Mandatory)] [System.Collections.IDictionary] $Actual,
        [Parameter(Mandatory)] [string] $ExpectedFileVersion,
        [Parameter(Mandatory)] [string] $ExpectedProductVersion,
        [Parameter(Mandatory)] [string] $Description
    )

    if ($Actual.file_version -ne $ExpectedFileVersion) {
        throw "$Description FileVersion is '$($Actual.file_version)', expected '$ExpectedFileVersion'."
    }
    if ($Actual.product_version -ne $ExpectedProductVersion) {
        throw "$Description ProductVersion is '$($Actual.product_version)', expected '$ExpectedProductVersion'."
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
$tagMatch = [regex]::Match(
    $ReleaseTag,
    '^v(?<major>0|[1-9][0-9]*)\.(?<minor>0|[1-9][0-9]*)\.(?<patch>0|[1-9][0-9]*)-build\.(?<revision>[1-9][0-9]*)$'
)
if (-not $tagMatch.Success) {
    throw "Release tag '$ReleaseTag' is not canonical."
}

$expectedFileVersion = '{0}.{1}.{2}.{3}' -f (
    $tagMatch.Groups['major'].Value,
    $tagMatch.Groups['minor'].Value,
    $tagMatch.Groups['patch'].Value,
    $tagMatch.Groups['revision'].Value
)
$expectedApplicationProductVersion = "$expectedFileVersion ($($sourceSha.Substring(0, 8)))"

Invoke-Git -Arguments @('fetch', '--force', 'origin', "refs/tags/${ReleaseTag}:refs/tags/${ReleaseTag}") | Out-Null
$headSha = (Invoke-Git -Arguments @('rev-parse', 'HEAD')).ToLowerInvariant()
if ($headSha -ne $sourceSha) {
    throw "Packaging checkout is $headSha, expected the validated source SHA $sourceSha."
}

$tagSha = (Invoke-Git -Arguments @('rev-parse', "$ReleaseTag^{commit}")).ToLowerInvariant()
if ($tagSha -ne $sourceSha) {
    throw "Release tag '$ReleaseTag' resolves to $tagSha, not the validated source SHA $sourceSha."
}

$revisionCountText = Invoke-Git -Arguments @('rev-list', '--count', '--no-merges', $sourceSha)
[Int64] $revisionCount = 0
if (-not [Int64]::TryParse($revisionCountText, [ref] $revisionCount)) {
    throw "Could not parse non-merge revision count '$revisionCountText'."
}
[Int64] $tagRevision = 0
if (-not [Int64]::TryParse($tagMatch.Groups['revision'].Value, [ref] $tagRevision)) {
    throw "Could not parse the build revision in '$ReleaseTag'."
}
if ($revisionCount -ne $tagRevision) {
    throw "Release tag '$ReleaseTag' declares build $tagRevision, but source $sourceSha has build $revisionCount."
}

$applicationPath = [IO.Path]::GetFullPath('x64/Release/sakura.exe')
$installerPath = Find-SingleFile -Directory 'installer/Output-x64' -Pattern 'sakura_install*.exe' -Description 'x64 installer'
$applicationVersion = Get-BinaryVersion -Path $applicationPath -Description 'Sakura executable'
$installerVersion = Get-BinaryVersion -Path $installerPath -Description 'Sakura installer'
Assert-BinaryVersion -Actual $applicationVersion -ExpectedFileVersion $expectedFileVersion -ExpectedProductVersion $expectedApplicationProductVersion -Description 'Sakura executable'
Assert-BinaryVersion -Actual $installerVersion -ExpectedFileVersion $expectedFileVersion -ExpectedProductVersion $expectedFileVersion -Description 'Sakura installer'

$application = Get-FileRecord -Path $applicationPath -Role 'application'
$application.file_version = $applicationVersion.file_version
$application.product_version = $applicationVersion.product_version
$installer = Get-FileRecord -Path $installerPath -Role 'installer'
$installer.file_version = $installerVersion.file_version
$installer.product_version = $installerVersion.product_version

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
        tag_name = $ReleaseTag
        sha = $sourceSha
        short_sha = $sourceSha.Substring(0, 8)
        revision_count = $revisionCount
        file_version = $expectedFileVersion
    }
    payload = [ordered]@{
        application = $application
        installer = $installer
        archives = @($archives)
    }
}

Write-Utf8NoBomFile -Path $OutputPath -Content ($result | ConvertTo-Json -Depth 8)
Write-Host "Recorded release provenance for $ReleaseTag at $sourceSha."
