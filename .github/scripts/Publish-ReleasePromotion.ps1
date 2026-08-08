[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidatePattern('^v(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)-build\.(?:[1-9][0-9]*)$')]
    [string] $TagName,

    [Parameter(Mandatory)]
    [ValidatePattern('^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$')]
    [string] $Repository,

    [Parameter(Mandatory)]
    [ValidateNotNullOrEmpty()]
    [string] $Token,

    [Parameter(Mandatory)] [string] $SourceEvidencePath,
    [Parameter(Mandatory)] [string] $ProvenancePath,
    [Parameter(Mandatory)] [string] $ArtifactRoot,
    [Parameter(Mandatory)] [string] $OutputDirectory,
    [Parameter(Mandatory)] [string] $ResultPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-RequiredStringProperty {
    param(
        [Parameter(Mandatory)] $Object,
        [Parameter(Mandatory)] [string] $Name,
        [Parameter(Mandatory)] [string] $Description
    )

    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) {
        throw "$Description is missing required property '$Name'."
    }
    $value = [string] $property.Value
    if ([string]::IsNullOrWhiteSpace($value)) {
        throw "$Description property '$Name' is empty."
    }
    return $value.Trim()
}

function Read-RequiredJson {
    param(
        [Parameter(Mandatory)] [string] $Path,
        [Parameter(Mandatory)] [string] $Description
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Description does not exist: $Path"
    }
    $json = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    if ($null -eq $json) {
        throw "$Description did not contain a JSON object: $Path"
    }
    return $json
}

function Assert-SafeLeafName {
    param(
        [Parameter(Mandatory)] [string] $Name,
        [Parameter(Mandatory)] [string] $Description
    )

    if ($Name -ne [IO.Path]::GetFileName($Name) -or
        $Name.IndexOfAny([IO.Path]::GetInvalidFileNameChars()) -ge 0) {
        throw "$Description is not a safe file name: '$Name'."
    }
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

function Assert-FileHash {
    param(
        [Parameter(Mandatory)] [string] $Path,
        [Parameter(Mandatory)] [string] $ExpectedSha256,
        [Parameter(Mandatory)] [string] $Description
    )

    if ($ExpectedSha256 -notmatch '^[0-9a-fA-F]{64}$') {
        throw "$Description has an invalid expected SHA-256 '$ExpectedSha256'."
    }
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Description does not exist: $Path"
    }
    $actual = Get-Sha256 -Path $Path
    if ($actual -ne $ExpectedSha256.ToLowerInvariant()) {
        throw "$Description SHA-256 is $actual, expected $ExpectedSha256."
    }
}

function Get-ArchiveRecord {
    param(
        [Parameter(Mandatory)] $Provenance,
        [Parameter(Mandatory)] [string] $Role
    )

    if ($null -eq $Provenance.payload -or $null -eq $Provenance.payload.archives) {
        throw 'Release provenance has no payload.archives collection.'
    }
    $matches = @($Provenance.payload.archives | Where-Object { $_.role -eq $Role })
    if ($matches.Count -ne 1) {
        throw "Expected exactly one '$Role' record in release provenance; found $($matches.Count)."
    }
    return $matches[0]
}

function Assert-PathWithin {
    param(
        [Parameter(Mandatory)] [string] $Path,
        [Parameter(Mandatory)] [string] $BasePath,
        [Parameter(Mandatory)] [string] $Description
    )

    $fullPath = [IO.Path]::GetFullPath($Path)
    $fullBase = [IO.Path]::GetFullPath($BasePath).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    if (-not $fullPath.StartsWith($fullBase, [StringComparison]::OrdinalIgnoreCase)) {
        throw "$Description is outside its owned directory: $fullPath"
    }
}

function Expand-VerifiedZip {
    param(
        [Parameter(Mandatory)] [string] $ArchivePath,
        [Parameter(Mandatory)] [string] $DestinationPath
    )

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $destination = [IO.Path]::GetFullPath($DestinationPath)
    [IO.Directory]::CreateDirectory($destination) | Out-Null
    $destinationPrefix = $destination.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    $archive = [IO.Compression.ZipFile]::OpenRead($ArchivePath)
    try {
        foreach ($entry in $archive.Entries) {
            $target = [IO.Path]::GetFullPath([IO.Path]::Combine($destination, $entry.FullName))
            if (-not $target.StartsWith($destinationPrefix, [StringComparison]::OrdinalIgnoreCase)) {
                throw "Archive entry escapes the extraction directory: $($entry.FullName)"
            }
            if ([string]::IsNullOrEmpty($entry.Name)) {
                [IO.Directory]::CreateDirectory($target) | Out-Null
                continue
            }

            $parent = Split-Path -Parent $target
            [IO.Directory]::CreateDirectory($parent) | Out-Null
            $input = $entry.Open()
            $output = $null
            try {
                $output = [IO.File]::Open($target, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
                $input.CopyTo($output)
            }
            finally {
                if ($null -ne $output) {
                    $output.Dispose()
                }
                $input.Dispose()
            }
        }
    }
    finally {
        $archive.Dispose()
    }
}

function Find-SingleFileRecursively {
    param(
        [Parameter(Mandatory)] [string] $RootPath,
        [Parameter(Mandatory)] [string] $LeafName,
        [Parameter(Mandatory)] [string] $Description
    )

    Assert-SafeLeafName -Name $LeafName -Description $Description
    $matches = @([IO.Directory]::EnumerateFiles(
        [IO.Path]::GetFullPath($RootPath),
        $LeafName,
        [IO.SearchOption]::AllDirectories
    ))
    if ($matches.Count -ne 1) {
        throw "Expected exactly one $Description named '$LeafName'; found $($matches.Count)."
    }
    return $matches[0]
}

function Get-ExistingRelease {
    param(
        [Parameter(Mandatory)] [string] $Tag,
        [Parameter(Mandatory)] [string] $Repo
    )

    $output = & gh api "repos/$Repo/releases/tags/$Tag" 2>&1
    $exitCode = $LASTEXITCODE
    if ($exitCode -eq 0) {
        return (($output | Out-String) | ConvertFrom-Json)
    }

    $diagnostic = ($output | Out-String)
    if ($diagnostic -match '(?i)\(HTTP 404\)') {
        return $null
    }
    throw "Could not determine whether release '$Tag' already exists in ${Repo}: $diagnostic"
}

function Get-ReleaseJson {
    param(
        [Parameter(Mandatory)] [string] $Tag,
        [Parameter(Mandatory)] [string] $Repo
    )

    $release = Get-ExistingRelease -Tag $Tag -Repo $Repo
    if ($null -eq $release) {
        throw "Release '$Tag' was not found in $Repo after draft creation."
    }
    return $release
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

$result = [ordered]@{
    schema = 1
    tag_name = $TagName
    status = 'running'
    draft_created = $false
    published = $false
    started_at = [DateTime]::UtcNow.ToString('o')
}
$primaryError = $null

try {
    $sourceEvidence = Read-RequiredJson -Path $SourceEvidencePath -Description 'Release source evidence'
    $provenance = Read-RequiredJson -Path $ProvenancePath -Description 'Release provenance'
    if ($null -eq $sourceEvidence -or $null -eq $provenance.source -or $null -eq $provenance.payload) {
        throw 'Release evidence is missing a required top-level object.'
    }

    $sourceTag = Get-RequiredStringProperty -Object $sourceEvidence -Name 'tag_name' -Description 'Release source evidence'
    $sourceSha = Get-RequiredStringProperty -Object $sourceEvidence -Name 'source_sha' -Description 'Release source evidence'
    $sourceVersion = Get-RequiredStringProperty -Object $sourceEvidence -Name 'file_version' -Description 'Release source evidence'
    $provenanceTag = Get-RequiredStringProperty -Object $provenance.source -Name 'tag_name' -Description 'Release provenance source'
    $provenanceSha = Get-RequiredStringProperty -Object $provenance.source -Name 'sha' -Description 'Release provenance source'
    $provenanceVersion = Get-RequiredStringProperty -Object $provenance.source -Name 'file_version' -Description 'Release provenance source'
    if ($TagName -ne $sourceTag -or $sourceTag -ne $provenanceTag -or
        $sourceSha.ToLowerInvariant() -ne $provenanceSha.ToLowerInvariant() -or
        $sourceVersion -ne $provenanceVersion) {
        throw 'Publication inputs do not identify the same validated tag, source SHA, and file version.'
    }

    $env:GH_TOKEN = $Token
    $existingRelease = Get-ExistingRelease -Tag $TagName -Repo $Repository
    if ($null -ne $existingRelease) {
        $state = if ($existingRelease.draft) { 'draft' } else { 'published' }
        throw "Release '$TagName' already exists as a $state release. Refusing to create or overwrite it."
    }

    $artifactRoot = [IO.Path]::GetFullPath($ArtifactRoot)
    if (-not [IO.Directory]::Exists($artifactRoot)) {
        throw "Downloaded release artifact root does not exist: $artifactRoot"
    }
    $outputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
    if ([IO.Directory]::Exists($outputDirectory) -or [IO.File]::Exists($outputDirectory)) {
        throw "Publication output directory already exists: $outputDirectory"
    }
    [IO.Directory]::CreateDirectory($outputDirectory) | Out-Null

    $archiveDefinitions = @(
        [pscustomobject]@{ role = 'installer_archive'; directory = 'installer' },
        [pscustomobject]@{ role = 'executable_archive'; directory = 'executable' },
        [pscustomobject]@{ role = 'log_archive'; directory = 'log' },
        [pscustomobject]@{ role = 'assembly_archive'; directory = 'assembly' },
        [pscustomobject]@{ role = 'development_archive'; directory = 'development' }
    )
    $assetPaths = [System.Collections.Generic.List[string]]::new()
    $archivePaths = @{}
    foreach ($definition in $archiveDefinitions) {
        $record = Get-ArchiveRecord -Provenance $provenance -Role $definition.role
        $name = Get-RequiredStringProperty -Object $record -Name 'name' -Description "$($definition.role) record"
        $hash = Get-RequiredStringProperty -Object $record -Name 'sha256' -Description "$($definition.role) record"
        Assert-SafeLeafName -Name $name -Description "$($definition.role) name"
        $sourcePath = Join-Path (Join-Path $artifactRoot $definition.directory) $name
        Assert-FileHash -Path $sourcePath -ExpectedSha256 $hash -Description $definition.role
        $destinationPath = Join-Path $outputDirectory $name
        [IO.File]::Copy($sourcePath, $destinationPath, $false)
        $assetPaths.Add($destinationPath)
        $archivePaths[$definition.role] = $destinationPath
    }

    $installer = $provenance.payload.installer
    if ($null -eq $installer) {
        throw 'Release provenance is missing installer metadata.'
    }
    $installerName = Get-RequiredStringProperty -Object $installer -Name 'name' -Description 'Installer record'
    $installerHash = Get-RequiredStringProperty -Object $installer -Name 'sha256' -Description 'Installer record'
    Assert-SafeLeafName -Name $installerName -Description 'Installer name'
    $extractionRoot = Join-Path $outputDirectory 'installer-extracted'
    Assert-PathWithin -Path $extractionRoot -BasePath $outputDirectory -Description 'Installer extraction directory'
    Expand-VerifiedZip -ArchivePath $archivePaths['installer_archive'] -DestinationPath $extractionRoot
    $installerPath = Find-SingleFileRecursively -RootPath $extractionRoot -LeafName $installerName -Description 'packaged installer'
    Assert-FileHash -Path $installerPath -ExpectedSha256 $installerHash -Description 'Packaged installer'
    $publishedInstallerPath = Join-Path $outputDirectory $installerName
    [IO.File]::Copy($installerPath, $publishedInstallerPath, $false)
    $assetPaths.Add($publishedInstallerPath)

    $publishedSourceEvidence = Join-Path $outputDirectory 'release-source.json'
    $publishedProvenance = Join-Path $outputDirectory 'release-provenance.json'
    [IO.File]::Copy([IO.Path]::GetFullPath($SourceEvidencePath), $publishedSourceEvidence, $false)
    [IO.File]::Copy([IO.Path]::GetFullPath($ProvenancePath), $publishedProvenance, $false)
    $assetPaths.Add($publishedSourceEvidence)
    $assetPaths.Add($publishedProvenance)

    $requiredCheckNames = @($sourceEvidence.required_checks | ForEach-Object {
        Get-RequiredStringProperty -Object $_ -Name 'name' -Description 'Required check evidence'
    } | Sort-Object)
    if ($requiredCheckNames.Count -eq 0) {
        throw 'Release source evidence did not record any required checks.'
    }
    $notesPath = Join-Path $outputDirectory 'release-notes.md'
    $notes = @(
        "## Verified release promotion",
        '',
        "- Source tag: ``$sourceTag``",
        "- Source SHA: ``$($sourceSha.ToLowerInvariant())``",
        "- File version: ``$sourceVersion``",
        '',
        'The source SHA passed the required main-branch checks before packaging. The packaged installer was provenance-checked, installed into an isolated directory, opened with a real document, and uninstalled before this release was published.',
        '',
        'Required source checks:',
        ($requiredCheckNames | ForEach-Object { "- $_" })
    ) -join "`n"
    Write-Utf8NoBomFile -Path $notesPath -Content $notes

    $expectedAssets = @{}
    foreach ($assetPath in $assetPaths) {
        $assetName = [IO.Path]::GetFileName($assetPath)
        $expectedAssets[$assetName] = Get-Sha256 -Path $assetPath
    }

    $createRequestPath = Join-Path $outputDirectory 'release-create-request.json'
    $createRequest = [ordered]@{
        tag_name = $TagName
        name = "Sakura Editor NEXT v$sourceVersion"
        body = $notes
        draft = $true
        prerelease = $false
    }
    Write-Utf8NoBomFile -Path $createRequestPath -Content ($createRequest | ConvertTo-Json -Depth 4)
    $createOutput = & gh api --method POST "repos/$Repository/releases" --header 'Content-Type: application/json' --input $createRequestPath
    if ($LASTEXITCODE -ne 0) {
        throw "Could not create draft release '$TagName'. The REST API leaves an existing release unchanged."
    }
    $release = (($createOutput | Out-String) | ConvertFrom-Json)
    if (-not $release.draft) {
        throw "Release '$TagName' was created without draft status. Refusing to upload artifacts."
    }
    $result.draft_created = $true

    & gh release upload $TagName --repo $Repository @assetPaths
    if ($LASTEXITCODE -ne 0) {
        throw "Could not upload all artifacts to draft release '$TagName'. It remains a draft."
    }

    $delaySeconds = 1
    $random = [Random]::new()
    $deadline = [DateTime]::UtcNow.AddMinutes(2)
    do {
        $release = Get-ReleaseJson -Tag $TagName -Repo $Repository
        if (-not $release.draft) {
            throw "Release '$TagName' became public before its uploaded-asset digests were verified."
        }

        $actualAssets = @{}
        foreach ($asset in @($release.assets)) {
            $actualAssets[$asset.name] = [string] $asset.digest
        }
        $missing = [System.Collections.Generic.List[string]]::new()
        $mismatches = [System.Collections.Generic.List[string]]::new()
        foreach ($expectedAsset in $expectedAssets.GetEnumerator()) {
            if (-not $actualAssets.ContainsKey($expectedAsset.Key)) {
                $missing.Add($expectedAsset.Key)
                continue
            }
            $actualDigest = $actualAssets[$expectedAsset.Key]
            $expectedDigest = "sha256:$($expectedAsset.Value)"
            if ($actualDigest -ne $expectedDigest) {
                $mismatches.Add("$($expectedAsset.Key): expected $expectedDigest, got $actualDigest")
            }
        }
        if ($missing.Count -eq 0 -and $mismatches.Count -eq 0) {
            break
        }
        if ([DateTime]::UtcNow -ge $deadline) {
            throw "Draft release asset verification timed out. Missing: $($missing -join ', '). Digest mismatches: $($mismatches -join '; ')"
        }
        $jitterMilliseconds = $random.Next(0, 251)
        Start-Sleep -Milliseconds (($delaySeconds * 1000) + $jitterMilliseconds)
        $delaySeconds = [Math]::Min($delaySeconds * 2, 8)
    } while ($true)

    & gh release edit $TagName --repo $Repository --draft=false
    if ($LASTEXITCODE -ne 0) {
        throw "Draft release '$TagName' passed provenance verification but could not be published. It remains a draft."
    }
    $release = Get-ReleaseJson -Tag $TagName -Repo $Repository
    if ($release.draft) {
        throw "Release '$TagName' still reports as a draft after the publish command."
    }

    $result.status = 'published'
    $result.published = $true
    $result.release_url = $release.html_url
    $result.source_sha = $sourceSha.ToLowerInvariant()
    $result.file_version = $sourceVersion
    $result.asset_digests = $expectedAssets
}
catch {
    $primaryError = $_
    $result.status = 'failed'
    $result.failure = $_.Exception.Message
}
finally {
    $result.finished_at = [DateTime]::UtcNow.ToString('o')
    Write-Utf8NoBomFile -Path $ResultPath -Content ($result | ConvertTo-Json -Depth 8)
}

if ($null -ne $primaryError) {
    throw $primaryError
}

Write-Host "Published verified release $TagName."
