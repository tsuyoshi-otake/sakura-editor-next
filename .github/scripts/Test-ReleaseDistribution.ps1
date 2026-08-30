[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $SourceEvidencePath,
    [Parameter(Mandatory)] [string] $ProvenancePath,
    [Parameter(Mandatory)] [string] $InstallerArchivePath,
    [Parameter(Mandatory)] [string] $ExecutableArchivePath,
    [Parameter(Mandatory)] [string] $EvidencePath,
    [ValidateRange(15, 300)] [int] $StartupTimeoutSeconds = 90
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

function Assert-ReleaseBuildContract {
    param([Parameter(Mandatory)] $Provenance)

    $contractProperties = @($Provenance.PSObject.Properties | Where-Object {
        $_.Name -ceq 'build_contract'
    })
    if ($contractProperties.Count -ne 1 -or $null -eq $contractProperties[0].Value) {
        throw "Release provenance is missing required property 'build_contract'."
    }
    $contract = $contractProperties[0].Value
    $expected = @(
        [pscustomobject]@{ Name = 'platform'; Value = 'x64' }
        [pscustomobject]@{ Name = 'configuration'; Value = 'Release' }
        [pscustomobject]@{ Name = 'utf16_backend'; Value = 'cpp' }
        [pscustomobject]@{ Name = 'output_backend'; Value = 'cpp' }
        [pscustomobject]@{ Name = 'utf16_production_package'; Value = 'true' }
        [pscustomobject]@{ Name = 'output_production_package'; Value = 'true' }
    )
    $properties = @($contract.PSObject.Properties)
    if ($properties.Count -ne $expected.Count) {
        throw "Release build contract must contain exactly $($expected.Count) properties."
    }
    for ($index = 0; $index -lt $expected.Count; ++$index) {
        $property = $properties[$index]
        $entry = $expected[$index]
        if ($property.Name -cne $entry.Name) {
            throw "Release build contract property $index is '$($property.Name)', expected exact name '$($entry.Name)'."
        }
        if (-not ($property.Value -is [string])) {
            throw "Release build contract property '$($entry.Name)' must be a string."
        }
        if ($property.Value -cne $entry.Value) {
            throw "Release build contract property '$($entry.Name)' is '$($property.Value)', expected exact value '$($entry.Value)'."
        }
    }
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

function Get-BinaryVersion {
    param(
        [Parameter(Mandatory)] [string] $Path,
        [Parameter(Mandatory)] [string] $Description
    )

    $version = [Diagnostics.FileVersionInfo]::GetVersionInfo($Path)
    if ([string]::IsNullOrWhiteSpace($version.FileVersion) -or
        [string]::IsNullOrWhiteSpace($version.ProductVersion)) {
        throw "$Description does not expose FileVersion and ProductVersion: $Path"
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

function Get-SmokeProcesses {
    param([Parameter(Mandatory)] [string] $InstalledExecutable)

    $expectedPath = [IO.Path]::GetFullPath($InstalledExecutable)
    return @(Get-CimInstance Win32_Process -Filter "Name = 'sakura.exe'" -ErrorAction Stop | Where-Object {
        $executablePath = [string] $_.ExecutablePath
        if ([string]::IsNullOrWhiteSpace($executablePath)) {
            return $false
        }
        try {
            return [IO.Path]::GetFullPath($executablePath).Equals(
                $expectedPath,
                [StringComparison]::OrdinalIgnoreCase
            )
        }
        catch {
            return $false
        }
    })
}

function Get-ProcessDepth {
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

function Stop-SmokeProcesses {
    param(
        [Parameter(Mandatory)] [string] $InstalledExecutable,
        [ValidateRange(1, 60)] [int] $WaitSeconds = 15
    )

    $survivors = @(Get-SmokeProcesses -InstalledExecutable $InstalledExecutable)
    if ($survivors.Count -eq 0) {
        return
    }

    $processById = @{}
    foreach ($process in $survivors) {
        $processById[[int] $process.ProcessId] = $process
    }
    $orderedSurvivors = @($survivors | ForEach-Object {
        [pscustomobject]@{
            depth = Get-ProcessDepth -Process $_ -ProcessById $processById
            process = $_
        }
    } | Sort-Object depth, @{ Expression = { [int] $_.process.ProcessId } })

    foreach ($entry in $orderedSurvivors) {
        $processId = [int] $entry.process.ProcessId
        $current = Get-CimInstance Win32_Process -Filter "ProcessId = $processId" -ErrorAction SilentlyContinue
        if ($null -eq $current) {
            continue
        }
        $stillOwned = @(Get-SmokeProcesses -InstalledExecutable $InstalledExecutable | Where-Object {
            [int] $_.ProcessId -eq $processId
        })
        if ($stillOwned.Count -eq 1) {
            Stop-Process -Id $processId -Force -ErrorAction Stop
        }
    }

    $remaining = @()
    $deadline = [DateTime]::UtcNow.AddSeconds($WaitSeconds)
    do {
        $remaining = @(Get-SmokeProcesses -InstalledExecutable $InstalledExecutable)
        if ($remaining.Count -eq 0) {
            return
        }
        Start-Sleep -Milliseconds 150
    } while ([DateTime]::UtcNow -lt $deadline)

    $details = $remaining |
        Select-Object ProcessId, ParentProcessId, Name, ExecutablePath, CommandLine |
        Format-Table -AutoSize |
        Out-String
    throw "Installed Sakura smoke process(es) survived cleanup:`n$details"
}

function Wait-ForSmokeWindow {
    param(
        [Parameter(Mandatory)] [string] $InstalledExecutable,
        [Parameter(Mandatory)] [string] $ExpectedDocumentName,
        [Parameter(Mandatory)] [int] $TimeoutSeconds
    )

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        foreach ($process in @(Get-SmokeProcesses -InstalledExecutable $InstalledExecutable)) {
            $managedProcess = Get-Process -Id ([int] $process.ProcessId) -ErrorAction SilentlyContinue
            if ($null -eq $managedProcess -or $managedProcess.MainWindowHandle -eq 0) {
                continue
            }
            $title = [string] $managedProcess.MainWindowTitle
            if ($title.IndexOf($ExpectedDocumentName, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
                return [ordered]@{
                    process_id = [int] $process.ProcessId
                    window_title = $title
                }
            }
        }
        Start-Sleep -Milliseconds 250
    } while ([DateTime]::UtcNow -lt $deadline)

    throw "Sakura did not open a main window for '$ExpectedDocumentName' within $TimeoutSeconds seconds."
}

function New-SmokeWorkDirectory {
    $baseDirectory = if ([string]::IsNullOrWhiteSpace($env:RUNNER_TEMP)) {
        [IO.Path]::GetTempPath()
    }
    else {
        $env:RUNNER_TEMP
    }
    $baseDirectory = [IO.Path]::GetFullPath($baseDirectory)
    if ($baseDirectory.Length -lt 4) {
        throw "Refusing to use an unsafe smoke-test temporary directory: '$baseDirectory'."
    }

    $workDirectory = Join-Path $baseDirectory "sakura-release-smoke-$([guid]::NewGuid().ToString('N'))"
    Assert-PathWithin -Path $workDirectory -BasePath $baseDirectory -Description 'Smoke work directory'
    [IO.Directory]::CreateDirectory($workDirectory) | Out-Null
    return $workDirectory
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

$startedAt = [DateTime]::UtcNow
$primaryError = $null
$cleanupErrors = [System.Collections.Generic.List[string]]::new()
$workDirectory = $null
$installDirectory = $null
$profileDirectory = $null
$installedExecutable = $null
$uninstallerPath = $null
$evidence = [ordered]@{
    schema = 1
    started_at = $startedAt.ToString('o')
    status = 'running'
}

try {
    $sourceEvidence = Read-RequiredJson -Path $SourceEvidencePath -Description 'Release source evidence'
    $provenance = Read-RequiredJson -Path $ProvenancePath -Description 'Release provenance'
    if ($null -eq $sourceEvidence -or $null -eq $provenance.source -or $null -eq $provenance.payload) {
        throw 'Release evidence is missing a required top-level object.'
    }
    Assert-ReleaseBuildContract -Provenance $provenance

    $sourceTag = Get-RequiredStringProperty -Object $sourceEvidence -Name 'tag_name' -Description 'Release source evidence'
    $sourceSha = Get-RequiredStringProperty -Object $sourceEvidence -Name 'source_sha' -Description 'Release source evidence'
    $sourceVersion = Get-RequiredStringProperty -Object $sourceEvidence -Name 'file_version' -Description 'Release source evidence'
    $provenanceTag = Get-RequiredStringProperty -Object $provenance.source -Name 'tag_name' -Description 'Release provenance source'
    $provenanceSha = Get-RequiredStringProperty -Object $provenance.source -Name 'sha' -Description 'Release provenance source'
    $provenanceVersion = Get-RequiredStringProperty -Object $provenance.source -Name 'file_version' -Description 'Release provenance source'
    if ($sourceTag -ne $provenanceTag -or
        $sourceSha.ToLowerInvariant() -ne $provenanceSha.ToLowerInvariant() -or
        $sourceVersion -ne $provenanceVersion) {
        throw 'Release provenance does not identify the source tag, SHA, and version validated before packaging.'
    }

    $installerArchive = Get-ArchiveRecord -Provenance $provenance -Role 'installer_archive'
    $executableArchive = Get-ArchiveRecord -Provenance $provenance -Role 'executable_archive'
    $installerArchiveName = Get-RequiredStringProperty -Object $installerArchive -Name 'name' -Description 'Installer archive record'
    $installerArchiveHash = Get-RequiredStringProperty -Object $installerArchive -Name 'sha256' -Description 'Installer archive record'
    $executableArchiveName = Get-RequiredStringProperty -Object $executableArchive -Name 'name' -Description 'Executable archive record'
    $executableArchiveHash = Get-RequiredStringProperty -Object $executableArchive -Name 'sha256' -Description 'Executable archive record'
    Assert-SafeLeafName -Name $installerArchiveName -Description 'Installer archive name'
    Assert-SafeLeafName -Name $executableArchiveName -Description 'Executable archive name'
    if ([IO.Path]::GetFileName($InstallerArchivePath) -ne $installerArchiveName -or
        [IO.Path]::GetFileName($ExecutableArchivePath) -ne $executableArchiveName) {
        throw 'Downloaded archive names do not match the release provenance.'
    }
    Assert-FileHash -Path $InstallerArchivePath -ExpectedSha256 $installerArchiveHash -Description 'Installer archive'
    Assert-FileHash -Path $ExecutableArchivePath -ExpectedSha256 $executableArchiveHash -Description 'Executable archive'

    $application = $provenance.payload.application
    $installer = $provenance.payload.installer
    if ($null -eq $application -or $null -eq $installer) {
        throw 'Release provenance is missing application or installer metadata.'
    }
    $applicationName = Get-RequiredStringProperty -Object $application -Name 'name' -Description 'Application record'
    $applicationHash = Get-RequiredStringProperty -Object $application -Name 'sha256' -Description 'Application record'
    $applicationFileVersion = Get-RequiredStringProperty -Object $application -Name 'file_version' -Description 'Application record'
    $applicationProductVersion = Get-RequiredStringProperty -Object $application -Name 'product_version' -Description 'Application record'
    $installerName = Get-RequiredStringProperty -Object $installer -Name 'name' -Description 'Installer record'
    $installerHash = Get-RequiredStringProperty -Object $installer -Name 'sha256' -Description 'Installer record'
    $installerFileVersion = Get-RequiredStringProperty -Object $installer -Name 'file_version' -Description 'Installer record'
    $installerProductVersion = Get-RequiredStringProperty -Object $installer -Name 'product_version' -Description 'Installer record'
    Assert-SafeLeafName -Name $applicationName -Description 'Application name'
    Assert-SafeLeafName -Name $installerName -Description 'Installer name'

    $workDirectory = New-SmokeWorkDirectory
    $installerExtractDirectory = Join-Path $workDirectory 'installer-archive'
    $executableExtractDirectory = Join-Path $workDirectory 'executable-archive'
    Expand-VerifiedZip -ArchivePath $InstallerArchivePath -DestinationPath $installerExtractDirectory
    Expand-VerifiedZip -ArchivePath $ExecutableArchivePath -DestinationPath $executableExtractDirectory
    $packagedInstaller = Find-SingleFileRecursively -RootPath $installerExtractDirectory -LeafName $installerName -Description 'packaged installer'
    $packagedApplication = Find-SingleFileRecursively -RootPath $executableExtractDirectory -LeafName $applicationName -Description 'packaged application'
    Assert-FileHash -Path $packagedInstaller -ExpectedSha256 $installerHash -Description 'Packaged installer'
    Assert-FileHash -Path $packagedApplication -ExpectedSha256 $applicationHash -Description 'Packaged application'
    Assert-BinaryVersion -Actual (Get-BinaryVersion -Path $packagedInstaller -Description 'Packaged installer') -ExpectedFileVersion $installerFileVersion -ExpectedProductVersion $installerProductVersion -Description 'Packaged installer'
    Assert-BinaryVersion -Actual (Get-BinaryVersion -Path $packagedApplication -Description 'Packaged application') -ExpectedFileVersion $applicationFileVersion -ExpectedProductVersion $applicationProductVersion -Description 'Packaged application'

    $installDirectory = Join-Path $workDirectory 'installed'
    Assert-PathWithin -Path $installDirectory -BasePath $workDirectory -Description 'Smoke installation directory'
    $installerProcess = Start-Process `
        -FilePath $packagedInstaller `
        -ArgumentList @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/SP-', "/DIR=`"$installDirectory`"") `
        -Wait `
        -PassThru
    if ($installerProcess.ExitCode -ne 0) {
        throw "Installer exited with code $($installerProcess.ExitCode)."
    }

    $installedExecutable = Join-Path $installDirectory $applicationName
    if (-not (Test-Path -LiteralPath $installedExecutable -PathType Leaf)) {
        throw "Installer completed but did not create $installedExecutable."
    }
    $uninstallerPath = Join-Path $installDirectory 'unins000.exe'
    if (-not (Test-Path -LiteralPath $uninstallerPath -PathType Leaf)) {
        throw "Installer completed but did not create $uninstallerPath."
    }
    Assert-FileHash -Path $installedExecutable -ExpectedSha256 $applicationHash -Description 'Installed application'
    Assert-BinaryVersion -Actual (Get-BinaryVersion -Path $installedExecutable -Description 'Installed application') -ExpectedFileVersion $applicationFileVersion -ExpectedProductVersion $applicationProductVersion -Description 'Installed application'

    $profileName = "release-smoke-$([guid]::NewGuid().ToString('N'))"
    $profileRoot = Join-Path ([Environment]::GetFolderPath([Environment+SpecialFolder]::ApplicationData)) 'sakura'
    $profileDirectory = Join-Path $profileRoot $profileName
    Assert-PathWithin -Path $profileDirectory -BasePath $profileRoot -Description 'Smoke profile directory'
    $sampleDocument = Join-Path $workDirectory 'release-smoke-input.txt'
    Write-Utf8NoBomFile -Path $sampleDocument -Content "release promotion smoke for $sourceSha`r`n"
    $launchProcess = Start-Process `
        -FilePath $installedExecutable `
        -ArgumentList @("-PROF=$profileName", "`"$sampleDocument`"") `
        -PassThru
    $window = Wait-ForSmokeWindow `
        -InstalledExecutable $installedExecutable `
        -ExpectedDocumentName ([IO.Path]::GetFileName($sampleDocument)) `
        -TimeoutSeconds $StartupTimeoutSeconds

    $evidence.release = [ordered]@{
        tag_name = $sourceTag
        source_sha = $sourceSha.ToLowerInvariant()
        file_version = $sourceVersion
    }
    $evidence.launch = [ordered]@{
        started_process_id = [int] $launchProcess.Id
        observed_window_process_id = [int] $window.process_id
        window_title = $window.window_title
    }
    $evidence.payload = [ordered]@{
        application_sha256 = $applicationHash.ToLowerInvariant()
        installer_sha256 = $installerHash.ToLowerInvariant()
    }
}
catch {
    $primaryError = $_
    $evidence.failure = $_.Exception.Message
}
finally {
    if ($null -ne $installedExecutable -and (Test-Path -LiteralPath $installedExecutable -PathType Leaf)) {
        try {
            Stop-SmokeProcesses -InstalledExecutable $installedExecutable
        }
        catch {
            $cleanupErrors.Add("Failed to stop installed Sakura process(es): $($_.Exception.Message)")
        }
    }

    if ($null -ne $uninstallerPath -and (Test-Path -LiteralPath $uninstallerPath -PathType Leaf)) {
        try {
            $uninstallerProcess = Start-Process `
                -FilePath $uninstallerPath `
                -ArgumentList @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/SP-') `
                -Wait `
                -PassThru
            if ($uninstallerProcess.ExitCode -ne 0) {
                throw "Uninstaller exited with code $($uninstallerProcess.ExitCode)."
            }
        }
        catch {
            $cleanupErrors.Add("Failed to uninstall the smoke payload: $($_.Exception.Message)")
        }
    }

    foreach ($directory in @($profileDirectory, $workDirectory)) {
        if ([string]::IsNullOrWhiteSpace($directory) -or -not [IO.Directory]::Exists($directory)) {
            continue
        }
        try {
            if ($directory -eq $profileDirectory) {
                $profileRoot = Join-Path ([Environment]::GetFolderPath([Environment+SpecialFolder]::ApplicationData)) 'sakura'
                Assert-PathWithin -Path $directory -BasePath $profileRoot -Description 'Smoke profile cleanup directory'
            }
            else {
                $tempRoot = if ([string]::IsNullOrWhiteSpace($env:RUNNER_TEMP)) { [IO.Path]::GetTempPath() } else { $env:RUNNER_TEMP }
                Assert-PathWithin -Path $directory -BasePath $tempRoot -Description 'Smoke work cleanup directory'
            }
            [IO.Directory]::Delete($directory, $true)
        }
        catch {
            $cleanupErrors.Add("Failed to remove smoke directory '$directory': $($_.Exception.Message)")
        }
    }

    $evidence.finished_at = [DateTime]::UtcNow.ToString('o')
    $evidence.cleanup_errors = @($cleanupErrors)
    $evidence.status = if ($null -ne $primaryError -or $cleanupErrors.Count -ne 0) { 'failed' } else { 'success' }
    try {
        Write-Utf8NoBomFile -Path $EvidencePath -Content ($evidence | ConvertTo-Json -Depth 8)
    }
    catch {
        $cleanupErrors.Add("Failed to write smoke evidence: $($_.Exception.Message)")
    }
}

if ($null -ne $primaryError) {
    throw $primaryError
}
if ($cleanupErrors.Count -ne 0) {
    throw "Release smoke cleanup failed:`n$($cleanupErrors -join "`n")"
}

Write-Host 'Release distribution smoke test completed successfully.'
