[CmdletBinding(DefaultParameterSetName = 'Import')]
param(
    [Parameter(Mandatory, ParameterSetName = 'Import')]
    [string] $PackageSourcePath,

    [Parameter(Mandatory, ParameterSetName = 'Import')]
    [string] $DownloadRoot,

    [Parameter(Mandatory, ParameterSetName = 'Import')]
    [string] $SourceRoot,

    [Parameter(Mandatory, ParameterSetName = 'Import')] [string] $Platform,
    [Parameter(Mandatory, ParameterSetName = 'Import')] [string] $Configuration,
    [Parameter(Mandatory, ParameterSetName = 'Import')] [string] $Utf16Backend,
    [Parameter(Mandatory, ParameterSetName = 'Import')] [string] $OutputBackend,
    [Parameter(Mandatory, ParameterSetName = 'Import')] [string] $Utf16ProductionPackage,
    [Parameter(Mandatory, ParameterSetName = 'Import')] [string] $OutputProductionPackage,

    [Parameter(Mandatory, ParameterSetName = 'SelfTest')]
    [switch] $SelfTest
)

# Lays out a reused main-push Release build where Write-ReleaseProvenance.ps1
# expects a freshly built one (#376). Nothing is taken on trust: the build
# record must name the run attempt Select-ReleasePackageSource.ps1 chose and
# carry exactly the release build contract, every archive must match its
# recorded size and SHA-256, and the bare executable and installer are taken
# from inside those archives and must match their recorded hashes too. Any
# mismatch fails the promotion; it never falls back to a rebuild.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:BuildRecordFileName = 'release-build-record.json'

function Get-ArchiveDefinitions {
    return @(
        [pscustomobject]@{ role = 'installer_archive'; pattern = '^sakura-[^\\/]+-Installer\.zip$' },
        [pscustomobject]@{ role = 'executable_archive'; pattern = '^sakura-[^\\/]+-Exe\.zip$' },
        [pscustomobject]@{ role = 'log_archive'; pattern = '^sakura-[^\\/]+-Log\.zip$' },
        [pscustomobject]@{ role = 'assembly_archive'; pattern = '^sakura-[^\\/]+-Asm\.zip$' },
        [pscustomobject]@{ role = 'development_archive'; pattern = '^sakura-[^\\/]+-Dev\.zip$' }
    )
}

function Get-ExactProperty {
    param(
        [Parameter(Mandatory)] [AllowNull()] $Object,
        [Parameter(Mandatory)] [string] $Name,
        [Parameter(Mandatory)] [string] $Description
    )

    if ($null -eq $Object) {
        throw "$Description is missing."
    }
    $properties = @($Object.PSObject.Properties | Where-Object { $_.Name -ceq $Name })
    if ($properties.Count -ne 1) {
        throw "$Description must contain exactly one '$Name' property."
    }
    return $properties[0].Value
}

function Get-ExactString {
    param(
        [Parameter(Mandatory)] [AllowNull()] $Object,
        [Parameter(Mandatory)] [string] $Name,
        [Parameter(Mandatory)] [string] $Description
    )

    $value = Get-ExactProperty -Object $Object -Name $Name -Description $Description
    if (-not ($value -is [string]) -or [string]::IsNullOrWhiteSpace($value)) {
        throw "$Description property '$Name' must be a non-empty string."
    }
    return $value
}

function Get-PositiveInteger {
    param(
        [Parameter(Mandatory)] [AllowNull()] $Object,
        [Parameter(Mandatory)] [string] $Name,
        [Parameter(Mandatory)] [string] $Description
    )

    $value = Get-ExactProperty -Object $Object -Name $Name -Description $Description
    if (-not ($value -is [int] -or $value -is [long]) -or [Int64] $value -le 0) {
        throw "$Description property '$Name' must be a positive integer."
    }
    return [Int64] $value
}

function Assert-PropertyNames {
    param(
        [Parameter(Mandatory)] [AllowNull()] $Object,
        [Parameter(Mandatory)] [string[]] $Names,
        [Parameter(Mandatory)] [string] $Description
    )

    if ($null -eq $Object) {
        throw "$Description is missing."
    }
    $actual = @($Object.PSObject.Properties | ForEach-Object { $_.Name })
    if (($actual -join '|') -cne ($Names -join '|')) {
        throw "$Description must contain exactly the properties $($Names -join ', '); found $($actual -join ', ')."
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

function Assert-RecordedFile {
    param(
        [Parameter(Mandatory)] [string] $Path,
        [Parameter(Mandatory)] $Entry,
        [Parameter(Mandatory)] [string] $Description
    )

    $file = [IO.FileInfo]::new($Path)
    if (-not $file.Exists) {
        throw "$Description does not exist: $Path"
    }
    if ([Int64] $file.Length -ne [Int64] $Entry.size_bytes) {
        throw "$Description is $($file.Length) bytes, but the build record says $($Entry.size_bytes)."
    }
    $actual = Get-Sha256 -Path $file.FullName
    if ($actual -cne $Entry.sha256) {
        throw "$Description SHA-256 is $actual, but the build record says $($Entry.sha256)."
    }
}

function Find-SingleFileRecursively {
    param(
        [Parameter(Mandatory)] [string] $RootPath,
        [Parameter(Mandatory)] [string] $LeafName,
        [Parameter(Mandatory)] [string] $Description
    )

    Assert-SafeLeafName -Name $LeafName -Description $Description
    $candidates = @([IO.Directory]::EnumerateFiles(
        [IO.Path]::GetFullPath($RootPath),
        $LeafName,
        [IO.SearchOption]::AllDirectories
    ))
    if ($candidates.Count -ne 1) {
        throw "Expected exactly one $Description named '$LeafName'; found $($candidates.Count)."
    }
    return $candidates[0]
}

# Creates the missing directories above a file and records each one, so a
# failed layout can remove exactly what it created.
function New-ParentDirectory {
    param(
        [Parameter(Mandatory)] [string] $FilePath,
        [Parameter(Mandatory)] [AllowEmptyCollection()] [Collections.Generic.List[string]] $Created
    )

    $missing = [Collections.Generic.List[string]]::new()
    $directory = [IO.Path]::GetDirectoryName($FilePath)
    while (-not [IO.Directory]::Exists($directory)) {
        if ([string]::IsNullOrEmpty($directory)) {
            throw "No existing directory contains $FilePath."
        }
        $missing.Insert(0, $directory)
        $directory = [IO.Path]::GetDirectoryName($directory)
    }
    foreach ($path in $missing) {
        [IO.Directory]::CreateDirectory($path) | Out-Null
        $Created.Add($path)
    }
}

# Removes, newest first, the files and directories a failed layout created.
# A directory that is no longer empty holds something this script did not
# write, so its removal fails instead of deleting it.
function Remove-CreatedPaths {
    param(
        [Parameter(Mandatory)] [AllowEmptyCollection()] [Collections.Generic.List[string]] $Created
    )

    for ($index = $Created.Count - 1; $index -ge 0; $index--) {
        $path = $Created[$index]
        if ([IO.File]::Exists($path)) {
            [IO.File]::Delete($path)
        }
        elseif ([IO.Directory]::Exists($path)) {
            [IO.Directory]::Delete($path, $false)
        }
    }
}

function Copy-NewFile {
    param(
        [Parameter(Mandatory)] [IO.Stream] $Source,
        [Parameter(Mandatory)] [string] $DestinationPath,
        [Parameter(Mandatory)] [AllowEmptyCollection()] [Collections.Generic.List[string]] $Created
    )

    $destination = [IO.Path]::GetFullPath($DestinationPath)
    if ([IO.File]::Exists($destination) -or [IO.Directory]::Exists($destination)) {
        throw "Reused file destination already exists: $destination"
    }
    New-ParentDirectory -FilePath $destination -Created $Created
    $output = [IO.File]::Open($destination, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    $Created.Add($destination)
    try {
        $Source.CopyTo($output)
    }
    finally {
        $output.Dispose()
    }
}

function Expand-SingleEntry {
    param(
        [Parameter(Mandatory)] [string] $ArchivePath,
        [Parameter(Mandatory)] [string] $LeafName,
        [Parameter(Mandatory)] [string] $DestinationPath,
        [Parameter(Mandatory)] [string] $Description,
        [Parameter(Mandatory)] [AllowEmptyCollection()] [Collections.Generic.List[string]] $Created
    )

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead($ArchivePath)
    try {
        $entries = @($archive.Entries | Where-Object { $_.Name -ceq $LeafName })
        if ($entries.Count -ne 1) {
            throw "Expected exactly one $Description entry named '$LeafName' in $ArchivePath; found $($entries.Count)."
        }
        $sourceStream = $entries[0].Open()
        try {
            Copy-NewFile -Source $sourceStream -DestinationPath $DestinationPath -Created $Created
        }
        finally {
            $sourceStream.Dispose()
        }
    }
    finally {
        $archive.Dispose()
    }
}

function Assert-FileEntry {
    param(
        [Parameter(Mandatory)] [AllowNull()] $Entry,
        [Parameter(Mandatory)] [string] $Role,
        [Parameter(Mandatory)] [string[]] $Names,
        [Parameter(Mandatory)] [string] $NamePattern,
        [Parameter(Mandatory)] [string] $Description
    )

    Assert-PropertyNames -Object $Entry -Names $Names -Description $Description
    if ((Get-ExactString -Object $Entry -Name 'role' -Description $Description) -cne $Role) {
        throw "$Description has role '$($Entry.role)', expected '$Role'."
    }
    $name = Get-ExactString -Object $Entry -Name 'name' -Description $Description
    Assert-SafeLeafName -Name $name -Description "$Description name"
    if ($name -cnotmatch $NamePattern) {
        throw "$Description name '$name' does not match $NamePattern."
    }
    if ((Get-ExactString -Object $Entry -Name 'sha256' -Description $Description) -cnotmatch '^[0-9a-f]{64}$') {
        throw "$Description has an invalid SHA-256 '$($Entry.sha256)'."
    }
    Get-PositiveInteger -Object $Entry -Name 'size_bytes' -Description $Description | Out-Null
}

function Assert-ReleaseBuildRecord {
    param(
        [Parameter(Mandatory)] $Record,
        [Parameter(Mandatory)] $PackageSource,
        [Parameter(Mandatory)] [object[]] $Contract
    )

    if ((Get-ExactProperty -Object $PackageSource -Name 'mode' -Description 'Release package source') -cne 'reuse') {
        throw "Release package source does not select reuse."
    }
    $sourceRun = Get-ExactProperty -Object $PackageSource -Name 'run' -Description 'Release package source'

    Assert-PropertyNames -Object $Record -Names @('schema', 'source', 'run', 'build_contract', 'payload') -Description 'Release build record'
    if ((Get-PositiveInteger -Object $Record -Name 'schema' -Description 'Release build record') -ne 1) {
        throw "Release build record schema is '$($Record.schema)', expected 1."
    }

    Assert-PropertyNames -Object $Record.source -Names @('sha') -Description 'Release build record source'
    $expectedSha = Get-ExactString -Object $PackageSource -Name 'source_sha' -Description 'Release package source'
    if ((Get-ExactString -Object $Record.source -Name 'sha' -Description 'Release build record source') -cne $expectedSha) {
        throw "Release build record describes source $($Record.source.sha), not $expectedSha."
    }

    Assert-PropertyNames -Object $Record.run -Names @('repository', 'id', 'attempt', 'event', 'ref') -Description 'Release build record run'
    $expectedRepository = Get-ExactString -Object $PackageSource -Name 'repository' -Description 'Release package source'
    if ((Get-ExactString -Object $Record.run -Name 'repository' -Description 'Release build record run') -cne $expectedRepository) {
        throw "Release build record was written in $($Record.run.repository), not $expectedRepository."
    }
    $expectedRunId = Get-PositiveInteger -Object $sourceRun -Name 'id' -Description 'Release package source run'
    $expectedAttempt = Get-PositiveInteger -Object $sourceRun -Name 'attempt' -Description 'Release package source run'
    if ((Get-PositiveInteger -Object $Record.run -Name 'id' -Description 'Release build record run') -ne $expectedRunId -or
        (Get-PositiveInteger -Object $Record.run -Name 'attempt' -Description 'Release build record run') -ne $expectedAttempt) {
        throw "Release build record comes from run $($Record.run.id) attempt $($Record.run.attempt), not run $expectedRunId attempt $expectedAttempt."
    }
    if ((Get-ExactString -Object $Record.run -Name 'event' -Description 'Release build record run') -cne 'push' -or
        (Get-ExactString -Object $Record.run -Name 'ref' -Description 'Release build record run') -cne 'refs/heads/main') {
        throw "Release build record was not written by a push to refs/heads/main."
    }

    $contractNames = @($Contract | ForEach-Object { $_.Name })
    Assert-PropertyNames -Object $Record.build_contract -Names $contractNames -Description 'Release build record build_contract'
    foreach ($entry in $Contract) {
        $value = Get-ExactProperty -Object $Record.build_contract -Name $entry.Name -Description 'Release build record build_contract'
        if (-not ($value -is [string]) -or $value -cne $entry.Value) {
            throw "Release build record build_contract '$($entry.Name)' is '$value', expected exact value '$($entry.Value)'."
        }
    }

    Assert-PropertyNames -Object $Record.payload -Names @('application', 'installer', 'archives') -Description 'Release build record payload'
    $binaryNames = @('role', 'name', 'sha256', 'size_bytes', 'file_version', 'product_version')
    Assert-FileEntry -Entry $Record.payload.application -Role 'application' -Names $binaryNames `
        -NamePattern '^sakura\.exe$' -Description 'Release build record application'
    Assert-FileEntry -Entry $Record.payload.installer -Role 'installer' -Names $binaryNames `
        -NamePattern '^sakura_install[^\\/]*\.exe$' -Description 'Release build record installer'
    $archives = @($Record.payload.archives)
    $definitions = @(Get-ArchiveDefinitions)
    if ($archives.Count -ne $definitions.Count) {
        throw "Release build record must list exactly $($definitions.Count) archives; found $($archives.Count)."
    }
    for ($index = 0; $index -lt $definitions.Count; ++$index) {
        Assert-FileEntry -Entry $archives[$index] -Role $definitions[$index].role -Names @('role', 'name', 'sha256', 'size_bytes') `
            -NamePattern $definitions[$index].pattern -Description "Release build record archive $index"
    }
}

function Install-ReleaseBuildRecord {
    param(
        [Parameter(Mandatory)] $Record,
        [Parameter(Mandatory)] [string] $DownloadRoot,
        [Parameter(Mandatory)] [string] $SourceRoot,
        [Parameter(Mandatory)] [string] $Platform,
        [Parameter(Mandatory)] [string] $Configuration
    )

    $sourceRootPath = [IO.Path]::GetFullPath($SourceRoot)
    # The layout is all or nothing. A failed check removes every file and
    # directory this call created, so a rejected package leaves no executable,
    # installer, or archive behind for a later step to mistake for the build.
    $created = [Collections.Generic.List[string]]::new()
    try {
        $archivePaths = @{}
        foreach ($archive in @($Record.payload.archives)) {
            $downloaded = Find-SingleFileRecursively -RootPath $DownloadRoot -LeafName $archive.name -Description $archive.role
            Assert-RecordedFile -Path $downloaded -Entry $archive -Description "Downloaded $($archive.role)"
            $destination = [IO.Path]::Combine($sourceRootPath, $archive.name)
            $sourceStream = [IO.File]::OpenRead($downloaded)
            try {
                Copy-NewFile -Source $sourceStream -DestinationPath $destination -Created $created
            }
            finally {
                $sourceStream.Dispose()
            }
            Assert-RecordedFile -Path $destination -Entry $archive -Description "Placed $($archive.role)"
            $archivePaths[$archive.role] = $destination
        }

        $application = $Record.payload.application
        $applicationPath = [IO.Path]::Combine($sourceRootPath, $Platform, $Configuration, $application.name)
        Expand-SingleEntry -ArchivePath $archivePaths['executable_archive'] -LeafName $application.name `
            -DestinationPath $applicationPath -Description 'application' -Created $created
        Assert-RecordedFile -Path $applicationPath -Entry $application -Description 'Reused application'

        $installer = $Record.payload.installer
        $installerPath = [IO.Path]::Combine($sourceRootPath, 'installer', "Output-$Platform", $installer.name)
        Expand-SingleEntry -ArchivePath $archivePaths['installer_archive'] -LeafName $installer.name `
            -DestinationPath $installerPath -Description 'installer' -Created $created
        Assert-RecordedFile -Path $installerPath -Entry $installer -Description 'Reused installer'
    }
    catch {
        $failure = $_
        try {
            Remove-CreatedPaths -Created $created
        }
        catch {
            throw "$($failure.Exception.Message) Removing the partial layout also failed: $($_.Exception.Message)"
        }
        throw $failure
    }
}

function Invoke-ImportSelfTest {
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem

    $sourceSha = 'a' * 40
    $contract = @(
        [pscustomobject]@{ Name = 'platform'; Value = 'x64' }
        [pscustomobject]@{ Name = 'configuration'; Value = 'Release' }
        [pscustomobject]@{ Name = 'utf16_backend'; Value = 'cpp' }
        [pscustomobject]@{ Name = 'output_backend'; Value = 'rust' }
        [pscustomobject]@{ Name = 'utf16_production_package'; Value = 'true' }
        [pscustomobject]@{ Name = 'output_production_package'; Value = 'true' }
    )
    $root = [IO.Path]::Combine([IO.Path]::GetTempPath(), "import-release-build-record-$([guid]::NewGuid().ToString('N'))")

    function Assert-True {
        param([bool] $Condition, [string] $Message)
        if (-not $Condition) { throw "Self-test failed: $Message" }
    }

    function Assert-Throws {
        param([scriptblock] $Action, [string] $ExpectedMessage)
        try {
            & $Action | Out-Null
        }
        catch {
            if (-not $_.Exception.Message.Contains($ExpectedMessage)) {
                throw "Self-test failed: expected '$ExpectedMessage', got '$($_.Exception.Message)'."
            }
            return
        }
        throw "Self-test failed: expected rejection '$ExpectedMessage'."
    }

    function Assert-NothingLaidOut {
        param([string] $Path, [string] $Message)
        Assert-True (@([IO.Directory]::EnumerateFileSystemEntries($Path)).Count -eq 0) $Message
    }

    function New-Zip {
        param([string] $Path, [hashtable] $Entries)
        $archive = [IO.Compression.ZipFile]::Open($Path, [IO.Compression.ZipArchiveMode]::Create)
        try {
            foreach ($key in $Entries.Keys) {
                $stream = $archive.CreateEntry($key).Open()
                try {
                    $bytes = [Text.Encoding]::UTF8.GetBytes($Entries[$key])
                    $stream.Write($bytes, 0, $bytes.Length)
                }
                finally {
                    $stream.Dispose()
                }
            }
        }
        finally {
            $archive.Dispose()
        }
    }

    function New-Entry {
        param([string] $Role, [string] $Path)
        return [ordered]@{
            role = $Role
            name = [IO.Path]::GetFileName($Path)
            sha256 = Get-Sha256 -Path $Path
            size_bytes = [Int64] ([IO.FileInfo]::new($Path).Length)
        }
    }

    # Builds one complete download tree and returns the record, as JSON text,
    # that Write-ReleaseBuildRecord.ps1 would have written for it.
    function New-Fixture {
        param([string] $Name)
        $fixture = [IO.Path]::Combine($root, $Name)
        $download = [IO.Path]::Combine($fixture, 'download')
        $staging = [IO.Path]::Combine($fixture, 'staging')
        $source = [IO.Path]::Combine($fixture, 'source')
        foreach ($directory in @($download, $staging, $source)) {
            [IO.Directory]::CreateDirectory($directory) | Out-Null
        }
        $prefix = 'sakura-build7900-aaaaaaaaa-x64-Release-alpha'
        $applicationPath = [IO.Path]::Combine($staging, 'sakura.exe')
        $installerPath = [IO.Path]::Combine($staging, 'sakura_install3-1-0-7900-x64.exe')
        [IO.File]::WriteAllText($applicationPath, 'application bytes')
        [IO.File]::WriteAllText($installerPath, 'installer bytes')
        $archives = foreach ($kind in @(
            @{ role = 'installer_archive'; artifact = 'Installer x64 Release'; suffix = 'Installer'; entries = @{ 'Installer/sakura_install3-1-0-7900-x64.exe' = 'installer bytes'; 'Installer/warning.txt' = 'w' } },
            @{ role = 'executable_archive'; artifact = 'Exe x64 Release'; suffix = 'Exe'; entries = @{ 'EXE/sakura.exe' = 'application bytes'; 'EXE/terminal-tools/tmux.exe' = 't' } },
            @{ role = 'log_archive'; artifact = 'Log x64 Release'; suffix = 'Log'; entries = @{ 'Log/msbuild.log' = 'l' } },
            @{ role = 'assembly_archive'; artifact = 'Asm x64 Release'; suffix = 'Asm'; entries = @{ 'Asm/a.asm' = 'a' } },
            @{ role = 'development_archive'; artifact = 'Dev x64 Release'; suffix = 'Dev'; entries = @{ 'DEV/sakura.pdb' = 'p' } }
        )) {
            $directory = [IO.Path]::Combine($download, $kind.artifact)
            [IO.Directory]::CreateDirectory($directory) | Out-Null
            $path = [IO.Path]::Combine($directory, "$prefix-$($kind.suffix).zip")
            New-Zip -Path $path -Entries $kind.entries
            New-Entry -Role $kind.role -Path $path
        }
        $application = New-Entry -Role 'application' -Path $applicationPath
        $application.file_version = '3.1.0.7900'
        $application.product_version = '3.1.0.7900 (aaaaaaaa)'
        $installer = New-Entry -Role 'installer' -Path $installerPath
        $installer.file_version = '3.1.0.7900'
        $installer.product_version = '3.1.0.7900'
        $record = [ordered]@{
            schema = 1
            source = [ordered]@{ sha = $sourceSha }
            run = [ordered]@{ repository = 'owner/repo'; id = [Int64] 700; attempt = [Int64] 2; event = 'push'; ref = 'refs/heads/main' }
            build_contract = [ordered]@{
                platform = 'x64'; configuration = 'Release'; utf16_backend = 'cpp'; output_backend = 'rust'
                utf16_production_package = 'true'; output_production_package = 'true'
            }
            payload = [ordered]@{ application = $application; installer = $installer; archives = @($archives) }
        }
        $recordDirectory = [IO.Path]::Combine($download, 'Release build record x64 Release')
        [IO.Directory]::CreateDirectory($recordDirectory) | Out-Null
        $json = $record | ConvertTo-Json -Depth 8
        [IO.File]::WriteAllText([IO.Path]::Combine($recordDirectory, $script:BuildRecordFileName), $json)
        return [pscustomobject]@{ download = $download; source = $source; json = $json }
    }

    function New-PackageSource {
        return [pscustomobject]@{
            mode = 'reuse'; repository = 'owner/repo'; source_sha = $sourceSha
            run = [pscustomobject]@{ id = [Int64] 700; attempt = [Int64] 2 }
        }
    }

    function Read-Record {
        param([string] $Json)
        return $Json | ConvertFrom-Json
    }

    try {
        $fixture = New-Fixture -Name 'complete'
        $record = Read-Record $fixture.json
        Assert-ReleaseBuildRecord -Record $record -PackageSource (New-PackageSource) -Contract $contract
        Install-ReleaseBuildRecord -Record $record -DownloadRoot $fixture.download -SourceRoot $fixture.source -Platform 'x64' -Configuration 'Release'
        Assert-True ([IO.File]::ReadAllText([IO.Path]::Combine($fixture.source, 'x64', 'Release', 'sakura.exe')) -ceq 'application bytes') 'the application is laid out where provenance reads it'
        Assert-True ([IO.File]::Exists([IO.Path]::Combine($fixture.source, 'installer', 'Output-x64', 'sakura_install3-1-0-7900-x64.exe'))) 'the installer is laid out where provenance reads it'
        Assert-True (@([IO.Directory]::EnumerateFiles($fixture.source, 'sakura-*.zip')).Count -eq 5) 'every archive is placed in the source root'
        Assert-Throws { Install-ReleaseBuildRecord -Record $record -DownloadRoot $fixture.download -SourceRoot $fixture.source -Platform 'x64' -Configuration 'Release' } 'already exists'
        Assert-True ([IO.File]::ReadAllText([IO.Path]::Combine($fixture.source, 'x64', 'Release', 'sakura.exe')) -ceq 'application bytes') 'a rejected layout keeps the files it did not create'
        Assert-True (@([IO.Directory]::EnumerateFiles($fixture.source, 'sakura-*.zip')).Count -eq 5) 'a rejected layout keeps the archives it did not create'

        foreach ($case in @(
            @{ mutate = { param($r) $r.schema = 2 }; expected = 'schema is' },
            @{ mutate = { param($r) $r.schema = '1' }; expected = "'schema' must be a positive integer" },
            @{ mutate = { param($r) $r.schema = $true }; expected = "'schema' must be a positive integer" },
            @{ mutate = { param($r) $r.source.sha = 'b' * 40 }; expected = 'describes source' },
            @{ mutate = { param($r) $r.run.repository = 'other/repo' }; expected = 'was written in other/repo' },
            @{ mutate = { param($r) $r.run.id = [Int64] 701 }; expected = 'comes from run 701 attempt 2' },
            @{ mutate = { param($r) $r.run.attempt = [Int64] 1 }; expected = 'comes from run 700 attempt 1' },
            @{ mutate = { param($r) $r.run.event = 'workflow_dispatch' }; expected = 'not written by a push' },
            @{ mutate = { param($r) $r.run.ref = 'refs/heads/feature' }; expected = 'not written by a push' },
            @{ mutate = { param($r) $r.build_contract.output_backend = 'cpp' }; expected = "'output_backend' is 'cpp'" },
            @{ mutate = { param($r) $r.build_contract.output_production_package = 'True' }; expected = "'output_production_package' is 'True'" },
            @{ mutate = { param($r) $r.build_contract | Add-Member -NotePropertyName extra -NotePropertyValue 'x' }; expected = 'build_contract must contain exactly' },
            @{ mutate = { param($r) $r.payload.application.name = 'other.exe' }; expected = 'does not match' },
            @{ mutate = { param($r) $r.payload.archives[1].name = '..\sakura-x-Exe.zip' }; expected = 'not a safe file name' },
            @{ mutate = { param($r) $r.payload.archives[1].role = 'log_archive' }; expected = "expected 'executable_archive'" },
            @{ mutate = { param($r) $r.payload.archives = @($r.payload.archives | Select-Object -First 4) }; expected = 'exactly 5 archives' },
            @{ mutate = { param($r) $r.payload.installer.sha256 = 'A' * 64 }; expected = 'invalid SHA-256' }
        )) {
            $mutated = Read-Record $fixture.json
            & $case.mutate $mutated
            Assert-Throws { Assert-ReleaseBuildRecord -Record $mutated -PackageSource (New-PackageSource) -Contract $contract } $case.expected
        }
        $rebuild = New-PackageSource
        $rebuild.mode = 'rebuild'
        Assert-Throws { Assert-ReleaseBuildRecord -Record (Read-Record $fixture.json) -PackageSource $rebuild -Contract $contract } 'does not select reuse'

        $fixture = New-Fixture -Name 'tampered-archive'
        $record = Read-Record $fixture.json
        $record.payload.archives[2].sha256 = 'f' * 64
        Assert-Throws { Install-ReleaseBuildRecord -Record $record -DownloadRoot $fixture.download -SourceRoot $fixture.source -Platform 'x64' -Configuration 'Release' } 'Downloaded log_archive SHA-256'
        Assert-NothingLaidOut $fixture.source 'a rejected archive removes the archives placed before it'

        $fixture = New-Fixture -Name 'tampered-application'
        $record = Read-Record $fixture.json
        $record.payload.application.size_bytes = [Int64] 999
        Assert-Throws { Install-ReleaseBuildRecord -Record $record -DownloadRoot $fixture.download -SourceRoot $fixture.source -Platform 'x64' -Configuration 'Release' } 'Reused application is'
        Assert-NothingLaidOut $fixture.source 'a rejected application leaves no layout'

        $fixture = New-Fixture -Name 'tampered-installer'
        $record = Read-Record $fixture.json
        $record.payload.installer.sha256 = 'f' * 64
        Assert-Throws { Install-ReleaseBuildRecord -Record $record -DownloadRoot $fixture.download -SourceRoot $fixture.source -Platform 'x64' -Configuration 'Release' } 'Reused installer SHA-256'
        Assert-NothingLaidOut $fixture.source 'a rejected installer removes the application laid out before it'

        $fixture = New-Fixture -Name 'duplicate-archive'
        $record = Read-Record $fixture.json
        $duplicate = [IO.Path]::Combine($fixture.download, 'Exe x64 Release', 'copy')
        [IO.Directory]::CreateDirectory($duplicate) | Out-Null
        $exeArchive = $record.payload.archives[1].name
        [IO.File]::Copy([IO.Path]::Combine($fixture.download, 'Exe x64 Release', $exeArchive), [IO.Path]::Combine($duplicate, $exeArchive))
        Assert-Throws { Install-ReleaseBuildRecord -Record $record -DownloadRoot $fixture.download -SourceRoot $fixture.source -Platform 'x64' -Configuration 'Release' } 'found 2'
        Assert-NothingLaidOut $fixture.source 'an ambiguous archive leaves no layout'
    }
    finally {
        if ([IO.Directory]::Exists($root)) {
            [IO.Directory]::Delete($root, $true)
        }
    }

    Write-Host 'Import-ReleaseBuildRecord self-test passed.'
}

if ($SelfTest) {
    Invoke-ImportSelfTest
    return
}

$contract = @(
    [pscustomobject]@{ Name = 'platform'; Value = $Platform }
    [pscustomobject]@{ Name = 'configuration'; Value = $Configuration }
    [pscustomobject]@{ Name = 'utf16_backend'; Value = $Utf16Backend }
    [pscustomobject]@{ Name = 'output_backend'; Value = $OutputBackend }
    [pscustomobject]@{ Name = 'utf16_production_package'; Value = $Utf16ProductionPackage }
    [pscustomobject]@{ Name = 'output_production_package'; Value = $OutputProductionPackage }
)
$packageSource = Get-Content -LiteralPath $PackageSourcePath -Raw | ConvertFrom-Json
$recordPath = Find-SingleFileRecursively -RootPath $DownloadRoot -LeafName $script:BuildRecordFileName -Description 'release build record'
$record = Get-Content -LiteralPath $recordPath -Raw | ConvertFrom-Json
Assert-ReleaseBuildRecord -Record $record -PackageSource $packageSource -Contract $contract
Install-ReleaseBuildRecord -Record $record -DownloadRoot $DownloadRoot -SourceRoot $SourceRoot -Platform $Platform -Configuration $Configuration
Write-Host "Reused the tested build of $($record.source.sha) from run $($record.run.id) attempt $($record.run.attempt)."
