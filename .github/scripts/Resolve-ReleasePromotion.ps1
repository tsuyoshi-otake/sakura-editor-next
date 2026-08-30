[CmdletBinding(DefaultParameterSetName = 'Resolve')]
param(
    [Parameter(Mandatory, ParameterSetName = 'Resolve')]
    [ValidatePattern('^v(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)-build\.(?:[1-9][0-9]*)$')]
    [string] $TagName,

    [Parameter(Mandatory, ParameterSetName = 'Resolve')]
    [ValidatePattern('^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$')]
    [string] $Repository,

    [Parameter(Mandatory, ParameterSetName = 'Resolve')]
    [ValidateNotNullOrEmpty()]
    [string] $Token,

    [Parameter(Mandatory, ParameterSetName = 'Resolve')]
    [string] $OutputPath,

    [Parameter(Mandatory, ParameterSetName = 'SelfTest')]
    [switch] $SelfTest
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

function Get-RequiredSourceCheckNames {
    return @(
        'check-encoding',
        'MSBuild (Debug, x64)',
        'MSBuild (Release, x64)',
        'cppcheck (x64, Release)',
        'doxygen (x64, Release)',
        'architecture-gates',
        'mingw (Debug)',
        'mingw (Release)',
        'Audit locked Rust dependencies'
    )
}

function Get-ExactReceiptProperty {
    param(
        [Parameter(Mandatory)] [object] $Object,
        [Parameter(Mandatory)] [string] $Name,
        [Parameter(Mandatory)] [string] $Description
    )

    $properties = @($Object.PSObject.Properties | Where-Object { $_.Name -ceq $Name })
    if ($properties.Count -ne 1) {
        throw "$Description must contain exactly one '$Name' property."
    }
    return $properties[0]
}

function ConvertFrom-PositiveNativeInteger {
    param(
        [AllowNull()] [object] $Value,
        [Parameter(Mandatory)] [string] $Description
    )

    if ($null -eq $Value) {
        throw "$Description must be a native positive integer."
    }
    $integerTypes = @(
        [byte], [sbyte], [int16], [uint16], [int32], [uint32], [int64], [uint64]
    )
    if ($integerTypes -notcontains $Value.GetType()) {
        throw "$Description must be a native positive integer."
    }
    if ([decimal] $Value -le 0 -or [decimal] $Value -gt [Int64]::MaxValue) {
        throw "$Description must be a native positive integer in the Int64 range."
    }
    return [Int64] $Value
}

function Get-ExactRequiredString {
    param(
        [Parameter(Mandatory)] [object] $Object,
        [Parameter(Mandatory)] [string] $Name,
        [Parameter(Mandatory)] [string] $Description
    )

    $property = Get-ExactReceiptProperty -Object $Object -Name $Name -Description $Description
    if (-not ($property.Value -is [string]) -or [string]::IsNullOrWhiteSpace($property.Value) -or
        $property.Value -cne $property.Value.Trim() -or $property.Value.Contains("`r") -or
        $property.Value.Contains("`n")) {
        throw "$Description property '$Name' must be an exact non-empty single-line string."
    }
    return [string] $property.Value
}

function ConvertFrom-StrictUtcTimestamp {
    param(
        [AllowNull()] [object] $Value,
        [Parameter(Mandatory)] [string] $Description
    )

    if (-not ($Value -is [string])) {
        throw "$Description must be an exact UTC timestamp string."
    }
    if ($Value -cnotmatch '^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}(?:\.[0-9]{1,7})?Z$') {
        throw "$Description must be an exact UTC timestamp string."
    }
    [DateTimeOffset] $parsed = [DateTimeOffset]::MinValue
    [string[]] $formats = @("yyyy-MM-dd'T'HH:mm:ss'Z'", "yyyy-MM-dd'T'HH:mm:ss.FFFFFFF'Z'")
    $styles = [Globalization.DateTimeStyles]::AssumeUniversal -bor [Globalization.DateTimeStyles]::AdjustToUniversal
    if (-not [DateTimeOffset]::TryParseExact(
        $Value,
        $formats,
        [Globalization.CultureInfo]::InvariantCulture,
        $styles,
        [ref] $parsed
    )) {
        throw "$Description must be an exact UTC timestamp string."
    }
    return $parsed
}

function ConvertTo-ValidatedCheckRuns {
    param(
        [Parameter(Mandatory)] [AllowEmptyCollection()] [object[]] $CheckRuns,
        [Parameter(Mandatory)] [string[]] $RequiredCheckNames,
        [Parameter(Mandatory)] [Int64] $TrustedAppId,
        [Parameter(Mandatory)] [string] $Repository,
        [Parameter(Mandatory)] [string] $SourceSha
    )

    if ($SourceSha -cnotmatch '^[0-9a-f]{40}$') {
        throw 'Expected source SHA must be exactly 40 lowercase hexadecimal characters.'
    }

    $validStatuses = @('queued', 'in_progress', 'completed', 'waiting', 'requested', 'pending')
    $validConclusions = @(
        'action_required', 'cancelled', 'failure', 'neutral', 'success', 'skipped',
        'stale', 'startup_failure', 'timed_out'
    )
    $validated = [System.Collections.Generic.List[object]]::new()
    for ($index = 0; $index -lt $CheckRuns.Count; ++$index) {
        $run = $CheckRuns[$index]
        $description = "Check-run receipt at index $index"
        if ($null -eq $run) {
            throw "$description must be an object."
        }

        $name = Get-ExactRequiredString -Object $run -Name 'name' -Description $description
        $idProperty = Get-ExactReceiptProperty -Object $run -Name 'id' -Description $description
        $id = ConvertFrom-PositiveNativeInteger -Value $idProperty.Value -Description "$description id"
        $headSha = Get-ExactRequiredString -Object $run -Name 'head_sha' -Description $description
        if ($headSha -cnotmatch '^[0-9a-f]{40}$' -or $headSha -cne $SourceSha) {
            throw "$description head_sha must exactly match the lowercase source SHA $SourceSha."
        }
        $appProperty = Get-ExactReceiptProperty -Object $run -Name 'app' -Description $description
        if ($null -eq $appProperty.Value -or $appProperty.Value -is [string] -or
            $appProperty.Value.GetType().IsPrimitive) {
            throw "$description property 'app' must be an object."
        }
        $appIdProperty = Get-ExactReceiptProperty -Object $appProperty.Value -Name 'id' -Description "$description app"
        $appId = ConvertFrom-PositiveNativeInteger -Value $appIdProperty.Value -Description "$description app id"
        $appSlug = Get-ExactRequiredString -Object $appProperty.Value -Name 'slug' -Description "$description app"
        if ($appId -eq $TrustedAppId -and $appSlug -cne 'github-actions') {
            throw "$description trusted app must have exact slug 'github-actions'."
        }
        $suiteProperty = Get-ExactReceiptProperty -Object $run -Name 'check_suite' -Description $description
        if ($null -eq $suiteProperty.Value -or $suiteProperty.Value -is [string] -or
            $suiteProperty.Value.GetType().IsPrimitive) {
            throw "$description property 'check_suite' must be an object."
        }
        $suiteIdProperty = Get-ExactReceiptProperty -Object $suiteProperty.Value -Name 'id' -Description "$description check_suite"
        $suiteId = ConvertFrom-PositiveNativeInteger -Value $suiteIdProperty.Value -Description "$description check_suite id"
        $suiteHeadShaProperties = @($suiteProperty.Value.PSObject.Properties | Where-Object { $_.Name -ceq 'head_sha' })
        if ($suiteHeadShaProperties.Count -gt 1) {
            throw "$description check_suite must contain at most one 'head_sha' property."
        }
        if ($suiteHeadShaProperties.Count -eq 1 -and $null -ne $suiteHeadShaProperties[0].Value) {
            throw "$description check_suite head_sha must be absent or null."
        }
        $status = Get-ExactRequiredString -Object $run -Name 'status' -Description $description
        if ($validStatuses -cnotcontains $status) {
            throw "$description has invalid exact status '$status'."
        }
        $detailsUrl = Get-ExactRequiredString -Object $run -Name 'details_url' -Description $description
        [Uri] $parsedDetailsUrl = $null
        if (-not [Uri]::TryCreate($detailsUrl, [UriKind]::Absolute, [ref] $parsedDetailsUrl) -or
            $parsedDetailsUrl.Scheme -cne 'https') {
            throw "$description property 'details_url' must be an absolute HTTPS URL."
        }
        $repositoryPattern = [regex]::Escape($Repository)
        $checkRunIdPattern = [regex]::Escape([string] $id)
        $actionsDetailsPattern = '^https://github\.com/' + $repositoryPattern +
            '/actions/runs/[1-9][0-9]*/job/' + $checkRunIdPattern + '$'
        $legacyDetailsPattern = '^https://github\.com/' + $repositoryPattern +
            '/runs/' + $checkRunIdPattern + '$'
        $isRequiredCheck = $RequiredCheckNames -ccontains $name
        $isCanonicalActionsJob = [regex]::IsMatch($detailsUrl, $actionsDetailsPattern)
        $isCanonicalLegacyRun = [regex]::IsMatch($detailsUrl, $legacyDetailsPattern)
        if (($isRequiredCheck -and -not $isCanonicalActionsJob) -or
            (-not $isRequiredCheck -and -not $isCanonicalActionsJob -and -not $isCanonicalLegacyRun)) {
            $expectedPath = if ($isRequiredCheck) { 'canonical Actions job URL' } else { 'canonical GitHub run URL' }
            throw "$description property 'details_url' must be a $expectedPath for $Repository and terminate with check-run id $id."
        }
        $startedAtProperty = Get-ExactReceiptProperty -Object $run -Name 'started_at' -Description $description
        $startedAt = ConvertFrom-StrictUtcTimestamp -Value $startedAtProperty.Value -Description "$description started_at"
        $conclusionProperty = Get-ExactReceiptProperty -Object $run -Name 'conclusion' -Description $description
        $completedAtProperty = Get-ExactReceiptProperty -Object $run -Name 'completed_at' -Description $description

        $externalIdProperties = @($run.PSObject.Properties | Where-Object { $_.Name -ceq 'external_id' })
        if ($externalIdProperties.Count -gt 1) {
            throw "$description must contain at most one 'external_id' property."
        }
        $externalId = $null
        if ($externalIdProperties.Count -eq 1) {
            $externalId = $externalIdProperties[0].Value
        }
        if ($isRequiredCheck) {
            if ($externalIdProperties.Count -ne 1 -or -not ($externalId -is [string]) -or
                [string]::IsNullOrWhiteSpace($externalId) -or $externalId -cne $externalId.Trim() -or
                $externalId.Contains("`r") -or $externalId.Contains("`n")) {
                throw "$description required Actions job must have a non-empty external_id UUID."
            }
            [Guid] $parsedExternalId = [Guid]::Empty
            if (-not [Guid]::TryParseExact($externalId, 'D', [ref] $parsedExternalId)) {
                throw "$description required Actions job external_id must be a UUID."
            }
        }
        $conclusion = $null
        $completedAt = $null
        if ($status -ceq 'completed') {
            if (-not ($conclusionProperty.Value -is [string]) -or
                $validConclusions -cnotcontains $conclusionProperty.Value) {
                throw "$description completed conclusion must be an exact supported string."
            }
            $conclusion = [string] $conclusionProperty.Value
            $completedAt = ConvertFrom-StrictUtcTimestamp -Value $completedAtProperty.Value `
                -Description "$description completed_at"
            if ($startedAt -gt $completedAt) {
                throw "$description started_at must not be later than completed_at."
            }
        }
        elseif ($null -ne $conclusionProperty.Value -or $null -ne $completedAtProperty.Value) {
            throw "$description non-completed run must have null conclusion and completed_at."
        }

        $validated.Add([pscustomobject]@{
            name = $name
            id = $id
            app = [pscustomobject]@{ id = $appId; slug = $appSlug }
            status = $status
            conclusion = $conclusion
            details_url = $detailsUrl
            head_sha = $headSha
            check_suite = [pscustomobject]@{ id = $suiteId; head_sha = if ($suiteHeadShaProperties.Count -eq 0) { $null } else { $suiteHeadShaProperties[0].Value } }
            external_id = $externalId
            started_at = [string] $startedAtProperty.Value
            completed_at = if ($null -eq $completedAtProperty.Value) { $null } else { [string] $completedAtProperty.Value }
            started_at_utc = $startedAt
        })
    }
    return @($validated)
}

function Resolve-RequiredSourceChecks {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]
        [AllowEmptyCollection()]
        [object[]] $CheckRuns,

        [Parameter(Mandatory)]
        [string[]] $RequiredCheckNames,

        [Parameter(Mandatory)]
        [Int64] $TrustedAppId,

        [Parameter(Mandatory)]
        [string] $Repository,

        [Parameter(Mandatory)]
        [string] $SourceSha
    )

    # Validate and normalize the complete API receipt before any name/app/state
    # filtering can evaluate or select a trusted match.
    $validatedRuns = @(ConvertTo-ValidatedCheckRuns -CheckRuns $CheckRuns `
        -RequiredCheckNames $RequiredCheckNames -TrustedAppId $TrustedAppId `
        -Repository $Repository -SourceSha $SourceSha)
    $verifiedChecks = [System.Collections.Generic.List[object]]::new()
    foreach ($requiredCheckName in $RequiredCheckNames) {
        $namedMatches = @($validatedRuns | Where-Object { $_.name -ceq $requiredCheckName })
        if ($namedMatches.Count -eq 0) {
            throw "Expected at least one check named '$requiredCheckName' for $SourceSha; found none."
        }

        $matches = @($namedMatches | Where-Object {
            $_.app.id -eq $TrustedAppId
        })
        if ($matches.Count -eq 0) {
            $observedAppIds = @($namedMatches | ForEach-Object {
                if ($_.app -eq $null) { 'missing' } else { [string] $_.app.id }
            }) -join ', '
            throw "Check '$requiredCheckName' for $SourceSha has no run from trusted GitHub Actions app $TrustedAppId. Observed app IDs: $observedAppIds"
        }

        # An active retry makes the immutable-SHA evidence unstable even when
        # an older successful run exists. Every trusted match must be terminal.
        $activeMatches = @($matches | Where-Object { $_.status -cne 'completed' })
        if ($activeMatches.Count -ne 0) {
            $states = ($activeMatches | ForEach-Object { "$($_.status)/$($_.conclusion)" }) -join ', '
            throw "Required source-gate check '$requiredCheckName' for $SourceSha still has $($activeMatches.Count) non-completed run(s). Observed: $states"
        }

        $successfulMatches = @($matches | Where-Object {
            $_.status -ceq 'completed' -and $_.conclusion -ceq 'success'
        })
        if ($successfulMatches.Count -eq 0) {
            $states = ($matches | ForEach-Object { "$($_.status)/$($_.conclusion)" }) -join ', '
            throw "Required source-gate check '$requiredCheckName' for $SourceSha has no completed success. Observed: $states"
        }

        $sortProperties = @(
            @{ Expression = { $_.started_at_utc }; Descending = $true },
            @{ Expression = { $_.id }; Descending = $true }
        )
        $check = @($successfulMatches | Sort-Object -Property $sortProperties | Select-Object -First 1)[0]
        $verifiedChecks.Add([ordered]@{
            name = $check.name
            check_run_id = [Int64] $check.id
            matching_check_run_count = $matches.Count
            successful_check_run_count = $successfulMatches.Count
            app_id = [Int64] $check.app.id
            status = $check.status
            conclusion = $check.conclusion
            details_url = $check.details_url
            started_at = $check.started_at
            completed_at = $check.completed_at
        })
    }
    return @($verifiedChecks)
}

function Invoke-SourceCheckSelfTest {
    $required = @(Get-RequiredSourceCheckNames)
    $trustedAppId = 15368
    $repository = 'tsuyoshi-otake/sakura-editor-next'
    $sourceSha = '0123456789abcdef0123456789abcdef01234567'

    function New-CheckRun {
        param(
            [AllowNull()] [object] $Name,
            [AllowNull()] [object] $Id,
            [AllowNull()] [object] $AppId = [Int64] 15368,
            [AllowNull()] [object] $Status = 'completed',
            [AllowNull()] [object] $Conclusion = 'success',
            [AllowNull()] [object] $StartedAt = '2026-08-30T00:00:00Z',
            [AllowNull()] [object] $DetailsUrl = $null,
            [AllowNull()] [object] $HeadSha = $null,
            [AllowNull()] [object] $AppSlug = $null,
            [AllowNull()] [object] $SuiteId = $null,
            [AllowNull()] [object] $SuiteHeadSha = $null,
            [AllowNull()] [object] $ExternalId = $null
        )
        if (-not $PSBoundParameters.ContainsKey('DetailsUrl')) {
            $DetailsUrl = "https://github.com/$repository/actions/runs/5000/job/$Id"
        }
        if (-not $PSBoundParameters.ContainsKey('HeadSha')) { $HeadSha = $sourceSha }
        if (-not $PSBoundParameters.ContainsKey('AppSlug')) { $AppSlug = 'github-actions' }
        if (-not $PSBoundParameters.ContainsKey('SuiteId')) { $SuiteId = [Int64] 9000 }
        if (-not $PSBoundParameters.ContainsKey('ExternalId')) {
            $ExternalId = '11111111-1111-4111-8111-111111111111'
        }
        $completedAt = if ($Status -ceq 'completed') { '2026-08-30T00:10:00Z' } else { $null }
        return [pscustomobject]@{
            name = $Name
            id = $Id
            head_sha = $HeadSha
            app = [pscustomobject]@{ id = $AppId; slug = $AppSlug }
            check_suite = [pscustomobject]@{ id = $SuiteId; head_sha = $SuiteHeadSha }
            status = $Status
            conclusion = $Conclusion
            details_url = $DetailsUrl
            external_id = $ExternalId
            started_at = $StartedAt
            completed_at = $completedAt
        }
    }

    function Assert-True {
        param([bool] $Condition, [string] $Message)
        if (-not $Condition) { throw "Self-test assertion failed: $Message" }
    }

    function Assert-Rejected {
        param([object[]] $Runs, [string] $ExpectedMessage)
        try {
            @(Resolve-RequiredSourceChecks -CheckRuns $Runs -RequiredCheckNames $required `
                -TrustedAppId $trustedAppId -Repository $repository -SourceSha $sourceSha) | Out-Null
        }
        catch {
            Assert-True -Condition ($_.Exception.Message -like "*$ExpectedMessage*") `
                -Message "expected rejection containing '$ExpectedMessage', got '$($_.Exception.Message)'"
            return
        }
        throw "Self-test assertion failed: expected rejection containing '$ExpectedMessage'."
    }

    $allSuccess = [System.Collections.Generic.List[object]]::new()
    for ($index = 0; $index -lt $required.Count; ++$index) {
        $allSuccess.Add((New-CheckRun -Name $required[$index] -Id (1000 + $index)))
    }
    $allSuccess[0].check_suite.PSObject.Properties.Remove('head_sha')
    $verified = @(Resolve-RequiredSourceChecks -CheckRuns @($allSuccess) `
        -RequiredCheckNames $required -TrustedAppId $trustedAppId `
        -Repository $repository -SourceSha $sourceSha)
    Assert-True ($verified.Count -eq 9) 'all nine trusted checks must be recorded'
    Assert-True (($verified.name -join '|') -eq ($required -join '|')) `
        'verified checks must retain the declared deterministic order'

    function New-RunsWithFirstReplacement {
        param([Parameter(Mandatory)] [object] $Replacement)
        $runs = @($allSuccess | Where-Object { $_.name -cne $required[0] })
        $runs += $Replacement
        return $runs
    }

    function Assert-MalformedFirstRunRejected {
        param(
            [Parameter(Mandatory)] [object] $Run,
            [Parameter(Mandatory)] [string] $ExpectedMessage
        )
        Assert-Rejected -Runs (New-RunsWithFirstReplacement -Replacement $Run) `
            -ExpectedMessage $ExpectedMessage
    }

    $missingId = New-CheckRun -Name $required[0] -Id 3000
    $missingId.PSObject.Properties.Remove('id')
    Assert-MalformedFirstRunRejected $missingId "exactly one 'id' property"
    foreach ($invalidId in @($null, '3000', $true, [double] 1.5, [Int64] 0, [Int64] -1, [UInt64]::MaxValue)) {
        Assert-MalformedFirstRunRejected `
            (New-CheckRun -Name $required[0] -Id $invalidId) 'native positive integer'
    }

    $missingAppId = New-CheckRun -Name $required[0] -Id 3001
    $missingAppId.app.PSObject.Properties.Remove('id')
    Assert-MalformedFirstRunRejected $missingAppId "exactly one 'id' property"
    foreach ($invalidAppId in @($null, '15368', $false, [decimal] 15368.0, [Int64] 0, [Int64] -1, [UInt64]::MaxValue)) {
        Assert-MalformedFirstRunRejected `
            (New-CheckRun -Name $required[0] -Id 3002 -AppId $invalidAppId) 'native positive integer'
    }

    $missingName = New-CheckRun -Name $required[0] -Id 3003
    $missingName.PSObject.Properties.Remove('name')
    Assert-MalformedFirstRunRejected $missingName "exactly one 'name' property"
    Assert-MalformedFirstRunRejected (New-CheckRun -Name $null -Id 3004) "property 'name'"
    Assert-MalformedFirstRunRejected (New-CheckRun -Name @('nested') -Id 3005) "property 'name'"
    Assert-MalformedFirstRunRejected (New-CheckRun -Name "bad`nname" -Id 3006) "property 'name'"

    foreach ($invalidStatus in @($null, 1, 'Completed', 'unknown')) {
        Assert-MalformedFirstRunRejected `
            (New-CheckRun -Name $required[0] -Id 3007 -Status $invalidStatus) 'status'
    }
    foreach ($invalidConclusion in @($null, 1, 'Success', 'unknown')) {
        Assert-MalformedFirstRunRejected `
            (New-CheckRun -Name $required[0] -Id 3008 -Conclusion $invalidConclusion) 'conclusion'
    }
    foreach ($invalidDetailsUrl in @(1, 'relative/path', 'http://example.invalid/check')) {
        Assert-MalformedFirstRunRejected `
            (New-CheckRun -Name $required[0] -Id 3009 -DetailsUrl $invalidDetailsUrl) 'details_url'
    }
    $missingDetailsUrl = New-CheckRun -Name $required[0] -Id 3015
    $missingDetailsUrl.PSObject.Properties.Remove('details_url')
    Assert-MalformedFirstRunRejected $missingDetailsUrl "exactly one 'details_url' property"
    $nullDetailsUrl = New-CheckRun -Name $required[0] -Id 3016
    $nullDetailsUrl.details_url = $null
    Assert-MalformedFirstRunRejected $nullDetailsUrl "property 'details_url'"

    $missingStartedAt = New-CheckRun -Name $required[0] -Id 3010
    $missingStartedAt.PSObject.Properties.Remove('started_at')
    Assert-MalformedFirstRunRejected $missingStartedAt "exactly one 'started_at' property"
    foreach ($invalidStartedAt in @(
        $null,
        1,
        'not-a-time',
        '2026-08-30T09:00:00+09:00',
        '2026-08-30T00:00:00.Z',
        '2026-08-30T00:00:00.12345678Z',
        '2026-08-30T00:00:00+00:00',
        '2026-08-30T00:00:00z',
        '2026-08-30T00:00:00Z ',
        ' 2026-08-30T00:00:00Z',
        '2026-02-30T00:00:00Z',
        '2026-08-30T24:00:00Z'
    )) {
        Assert-MalformedFirstRunRejected `
            (New-CheckRun -Name $required[0] -Id 3011 -StartedAt $invalidStartedAt) 'started_at'
    }
    foreach ($fractionalStartedAt in @(
        '2026-08-30T00:00:00.1Z',
        '2026-08-30T00:00:00.1234567Z'
    )) {
        $fractionalRun = @(Resolve-RequiredSourceChecks `
            -CheckRuns (New-RunsWithFirstReplacement -Replacement (
                New-CheckRun -Name $required[0] -Id 3033 -StartedAt $fractionalStartedAt
            )) `
            -RequiredCheckNames $required -TrustedAppId $trustedAppId `
            -Repository $repository -SourceSha $sourceSha)
        Assert-True ($fractionalRun[0].started_at -ceq $fractionalStartedAt) `
            'one- and seven-digit fractional timestamps must be accepted'
    }

    $completedAtNull = New-CheckRun -Name $required[0] -Id 3012
    $completedAtNull.completed_at = $null
    Assert-MalformedFirstRunRejected $completedAtNull 'completed_at'
    $completedAtNotTime = New-CheckRun -Name $required[0] -Id 3013
    $completedAtNotTime.completed_at = 'not-a-time'
    Assert-MalformedFirstRunRejected $completedAtNotTime 'completed_at'
    $completedBeforeStart = New-CheckRun -Name $required[0] -Id 3014 `
        -StartedAt '2026-08-30T00:20:00Z'
    Assert-MalformedFirstRunRejected $completedBeforeStart 'must not be later than completed_at'

    $malformedUnrelated = New-CheckRun -Name 'unrelated-check' -Id 'numeric-string'
    Assert-Rejected -Runs (@($allSuccess) + $malformedUnrelated) `
        -ExpectedMessage 'native positive integer'

    foreach ($invalidHeadSha in @($null, $sourceSha.ToUpperInvariant(), $sourceSha.Replace('1', '2'))) {
        Assert-MalformedFirstRunRejected `
            (New-CheckRun -Name $required[0] -Id 3017 -HeadSha $invalidHeadSha) 'head_sha'
    }
    $missingHeadSha = New-CheckRun -Name $required[0] -Id 3018
    $missingHeadSha.PSObject.Properties.Remove('head_sha')
    Assert-MalformedFirstRunRejected $missingHeadSha "exactly one 'head_sha' property"

    $missingAppSlug = New-CheckRun -Name $required[0] -Id 3019
    $missingAppSlug.app.PSObject.Properties.Remove('slug')
    Assert-MalformedFirstRunRejected $missingAppSlug "exactly one 'slug' property"
    Assert-MalformedFirstRunRejected `
        (New-CheckRun -Name $required[0] -Id 3020 -AppSlug 'GitHub-Actions') 'exact slug'

    $missingSuite = New-CheckRun -Name $required[0] -Id 3021
    $missingSuite.PSObject.Properties.Remove('check_suite')
    Assert-MalformedFirstRunRejected $missingSuite "exactly one 'check_suite' property"
    $malformedSuite = New-CheckRun -Name $required[0] -Id 3022
    $malformedSuite.check_suite = '9000'
    Assert-MalformedFirstRunRejected $malformedSuite 'check_suite'
    $missingSuiteId = New-CheckRun -Name $required[0] -Id 3023
    $missingSuiteId.check_suite.PSObject.Properties.Remove('id')
    Assert-MalformedFirstRunRejected $missingSuiteId "exactly one 'id' property"
    foreach ($invalidSuiteId in @($null, '9000', [Int64] 0, [Int64] -1, $true, [double] 1.5)) {
        Assert-MalformedFirstRunRejected `
            (New-CheckRun -Name $required[0] -Id 3024 -SuiteId $invalidSuiteId) 'check_suite id'
    }
    Assert-MalformedFirstRunRejected `
        (New-CheckRun -Name $required[0] -Id 3025 -SuiteHeadSha $sourceSha) 'absent or null'

    Assert-MalformedFirstRunRejected `
        (New-CheckRun -Name $required[0] -Id 3026 `
            -DetailsUrl "https://gitlab.com/$repository/actions/runs/5000/job/3026") 'canonical'
    Assert-MalformedFirstRunRejected `
        (New-CheckRun -Name $required[0] -Id 3027 `
            -DetailsUrl 'https://github.com/other/repo/actions/runs/5000/job/3027') 'canonical'
    Assert-MalformedFirstRunRejected `
        (New-CheckRun -Name $required[0] -Id 3028 `
            -DetailsUrl "https://github.com/$repository/runs/3028") 'canonical Actions job URL'
    Assert-MalformedFirstRunRejected `
        (New-CheckRun -Name $required[0] -Id 3029 `
            -DetailsUrl "https://github.com/$repository/actions/runs/5000/job/9999") 'terminate with check-run id'

    $missingExternalId = New-CheckRun -Name $required[0] -Id 3030
    $missingExternalId.PSObject.Properties.Remove('external_id')
    Assert-MalformedFirstRunRejected $missingExternalId 'external_id'
    Assert-MalformedFirstRunRejected `
        (New-CheckRun -Name $required[0] -Id 3031 -ExternalId $null) 'external_id'
    foreach ($invalidExternalId in @('', 'not-a-uuid', 1)) {
        Assert-MalformedFirstRunRejected `
            (New-CheckRun -Name $required[0] -Id 3032 -ExternalId $invalidExternalId) 'external_id'
    }

    foreach ($missingName in $required) {
        $withoutOne = @($allSuccess | Where-Object { $_.name -cne $missingName })
        Assert-Rejected -Runs $withoutOne -ExpectedMessage "'$missingName'"
    }

    $wrongCaseName = @($allSuccess | Where-Object { $_.name -cne $required[0] })
    $wrongCaseName += New-CheckRun -Name $required[0].ToUpperInvariant() -Id 1999
    Assert-Rejected -Runs $wrongCaseName -ExpectedMessage "'$($required[0])'"

    $wrongApp = @($allSuccess | Where-Object { $_.name -cne $required[0] })
    $wrongApp += New-CheckRun -Name $required[0] -Id 2000 -AppId 99
    Assert-Rejected -Runs $wrongApp -ExpectedMessage 'no run from trusted GitHub Actions app 15368'

    foreach ($activeStatus in @('queued', 'in_progress')) {
        $activeRetry = @($allSuccess)
        $activeRetry += New-CheckRun -Name $required[0] -Id 2001 -Status $activeStatus -Conclusion $null
        Assert-Rejected -Runs $activeRetry -ExpectedMessage 'non-completed run(s)'
    }

    $wrongCaseStatus = @($allSuccess | Where-Object { $_.name -cne $required[0] })
    $wrongCaseStatus += New-CheckRun -Name $required[0] -Id 2006 -Status 'Completed'
    Assert-Rejected -Runs $wrongCaseStatus -ExpectedMessage 'status'

    foreach ($terminalConclusion in @('failure', 'cancelled', 'skipped')) {
        $failureOnly = @($allSuccess | Where-Object { $_.name -cne $required[0] })
        $failureOnly += New-CheckRun -Name $required[0] -Id 2002 -Conclusion $terminalConclusion
        Assert-Rejected -Runs $failureOnly -ExpectedMessage 'has no completed success'
    }

    $wrongCaseConclusion = @($allSuccess | Where-Object { $_.name -cne $required[0] })
    $wrongCaseConclusion += New-CheckRun -Name $required[0] -Id 2007 -Conclusion 'Success'
    Assert-Rejected -Runs $wrongCaseConclusion -ExpectedMessage 'conclusion'

    $olderFailureAndSuccess = @($allSuccess)
    $olderFailureAndSuccess += New-CheckRun -Name $required[0] -Id 2003 -Conclusion 'failure' `
        -StartedAt '2026-08-29T00:00:00Z'
    $withFailure = @(Resolve-RequiredSourceChecks -CheckRuns $olderFailureAndSuccess `
        -RequiredCheckNames $required -TrustedAppId $trustedAppId `
        -Repository $repository -SourceSha $sourceSha)
    Assert-True ($withFailure[0].check_run_id -eq 1000) `
        'an older terminal failure must not erase a successful run'

    $duplicateSuccess = @($allSuccess)
    $duplicateSuccess += New-CheckRun -Name $required[0] -Id 2004 `
        -StartedAt '2026-08-30T00:05:00Z'
    $newest = @(Resolve-RequiredSourceChecks -CheckRuns $duplicateSuccess `
        -RequiredCheckNames $required -TrustedAppId $trustedAppId `
        -Repository $repository -SourceSha $sourceSha)
    Assert-True ($newest[0].check_run_id -eq 2004) `
        'the newest successful trusted run must be selected deterministically'
    Assert-True ($newest[0].matching_check_run_count -eq 2) `
        'duplicate trusted matches must be counted'
    Assert-True ($newest[0].successful_check_run_count -eq 2) `
        'duplicate trusted successes must be counted'

    $withUnrelated = @($allSuccess)
    $withUnrelated += New-CheckRun -Name 'Test Results' -Id 2005 -AppId $trustedAppId `
        -DetailsUrl "https://github.com/$repository/runs/2005" -ExternalId ''
    $unrelatedResult = @(Resolve-RequiredSourceChecks -CheckRuns $withUnrelated `
        -RequiredCheckNames $required -TrustedAppId $trustedAppId `
        -Repository $repository -SourceSha $sourceSha)
    Assert-True ($unrelatedResult.Count -eq 9) 'unrelated checks must not change the evidence set'

    Write-Host 'Resolve-ReleasePromotion source-check self-test passed.'
}

if ($SelfTest) {
    Invoke-SourceCheckSelfTest
    return
}

$tagMatch = [regex]::Match(
    $TagName,
    '^v(?<major>0|[1-9][0-9]*)\.(?<minor>0|[1-9][0-9]*)\.(?<patch>0|[1-9][0-9]*)-build\.(?<revision>[1-9][0-9]*)$'
)
if (-not $tagMatch.Success) {
    throw "Release tag '$TagName' is not a canonical v<major>.<minor>.<patch>-build.<revision> tag."
}

$tagRef = "refs/tags/$TagName"
Invoke-Git -Arguments @('fetch', '--force', 'origin', "${tagRef}:${tagRef}") | Out-Null
$sourceSha = Invoke-Git -Arguments @('rev-parse', "${TagName}^{commit}")
$sourceSha = $sourceSha.ToLowerInvariant()
if ($sourceSha -notmatch '^[0-9a-f]{40}$') {
    throw "Tag '$TagName' resolved to an invalid commit SHA '$sourceSha'."
}

Invoke-Git -Arguments @('fetch', '--no-tags', 'origin', '+refs/heads/main:refs/remotes/origin/main') | Out-Null
& git merge-base --is-ancestor $sourceSha 'origin/main'
if ($LASTEXITCODE -ne 0) {
    throw "Tag '$TagName' resolves to $sourceSha, which is not reachable from origin/main."
}

$revisionCountText = Invoke-Git -Arguments @('rev-list', '--count', '--no-merges', $sourceSha)
[Int64] $revisionCount = 0
if (-not [Int64]::TryParse($revisionCountText, [ref] $revisionCount)) {
    throw "Could not parse the non-merge revision count '$revisionCountText' for $sourceSha."
}
[Int64] $tagRevision = 0
if (-not [Int64]::TryParse($tagMatch.Groups['revision'].Value, [ref] $tagRevision)) {
    throw "Could not parse the revision in release tag '$TagName'."
}
if ($revisionCount -ne $tagRevision) {
    throw "Release tag '$TagName' declares build $tagRevision, but $sourceSha has non-merge revision count $revisionCount."
}

$apiRoot = if ([string]::IsNullOrWhiteSpace($env:GITHUB_API_URL)) { 'https://api.github.com' } else { $env:GITHUB_API_URL.TrimEnd('/') }
$headers = @{
    Accept = 'application/vnd.github+json'
    Authorization = "Bearer $Token"
    'X-GitHub-Api-Version' = '2022-11-28'
}
$checkRunsUri = "$apiRoot/repos/$Repository/commits/$sourceSha/check-runs?per_page=100&filter=latest"
$checkRunsResponse = Invoke-RestMethod -Method Get -Uri $checkRunsUri -Headers $headers

# Validate the complete trusted-main source gate. MinGW remains advisory for
# pull requests and ordinary branch protection, but both trusted main-push
# configurations are mandatory evidence at the release boundary.
$requiredCheckNames = @(Get-RequiredSourceCheckNames)
$githubActionsAppId = 15368
$verifiedChecks = @(Resolve-RequiredSourceChecks -CheckRuns @($checkRunsResponse.check_runs) `
    -RequiredCheckNames $requiredCheckNames -TrustedAppId $githubActionsAppId `
    -Repository $Repository -SourceSha $sourceSha)

$fileVersion = '{0}.{1}.{2}.{3}' -f (
    $tagMatch.Groups['major'].Value,
    $tagMatch.Groups['minor'].Value,
    $tagMatch.Groups['patch'].Value,
    $tagMatch.Groups['revision'].Value
)
$result = [ordered]@{
    schema = 1
    tag_name = $TagName
    source_sha = $sourceSha
    short_source_sha = $sourceSha.Substring(0, 8)
    source_revision_count = $revisionCount
    file_version = $fileVersion
    main_ref = 'origin/main'
    required_checks = @($verifiedChecks)
}

Write-Utf8NoBomFile -Path $OutputPath -Content ($result | ConvertTo-Json -Depth 8)
Write-Host "Validated release source $sourceSha for $TagName ($fileVersion)."
