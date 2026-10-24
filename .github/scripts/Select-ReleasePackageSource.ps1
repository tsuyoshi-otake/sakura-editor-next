[CmdletBinding(DefaultParameterSetName = 'Select')]
param(
    [Parameter(Mandatory, ParameterSetName = 'Select')]
    [string] $SourceEvidencePath,

    [Parameter(Mandatory, ParameterSetName = 'Select')]
    [ValidatePattern('^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$')]
    [string] $Repository,

    [Parameter(Mandatory, ParameterSetName = 'Select')]
    [ValidateNotNullOrEmpty()]
    [string] $Token,

    [Parameter(Mandatory, ParameterSetName = 'Select')]
    [ValidateSet('true', 'false')]
    [string] $RebuildRequested,

    [Parameter(Mandatory, ParameterSetName = 'Select')]
    [string] $OutputPath,

    [Parameter(Mandatory, ParameterSetName = 'SelfTest')]
    [switch] $SelfTest
)

# Decides how release promotion obtains its package (#376). The resolver has
# already proven which main-push MSBuild (Release, x64) job tested the source.
# When that job's run still holds its build record and every archive, exactly
# once and unexpired, promotion reuses those tested bytes; when the evidence is
# merely absent (a source that predates the record, expired artifacts, a run
# that was not a main push), it rebuilds as before. Evidence that contradicts
# the resolver's receipt is never a reason to rebuild: it fails closed.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:ReleaseBuildCheckName = 'MSBuild (Release, x64)'
$script:ReleaseBuildWorkflowPath = '.github/workflows/build-sakura.yml'

function Get-ReusedArtifactNames {
    return @(
        'Release build record x64 Release',
        'Installer x64 Release',
        'Exe x64 Release',
        'Log x64 Release',
        'Asm x64 Release',
        'Dev x64 Release'
    )
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

function Get-ReleaseBuildCheckLocation {
    param(
        [Parameter(Mandatory)] [string] $Repository,
        [Parameter(Mandatory)] [AllowEmptyCollection()] [object[]] $RequiredChecks
    )

    $candidates = @($RequiredChecks | Where-Object {
        $null -ne $_ -and @($_.PSObject.Properties | Where-Object { $_.Name -ceq 'name' -and $_.Value -ceq $script:ReleaseBuildCheckName }).Count -eq 1
    })
    if ($candidates.Count -ne 1) {
        throw "Release source evidence must name exactly one '$($script:ReleaseBuildCheckName)' check; found $($candidates.Count)."
    }
    $check = $candidates[0]
    $description = "Required check '$($script:ReleaseBuildCheckName)'"
    $checkRunId = Get-PositiveInteger -Object $check -Name 'check_run_id' -Description $description
    $detailsUrl = Get-ExactString -Object $check -Name 'details_url' -Description $description
    $pattern = '^https://github\.com/' + [regex]::Escape($Repository) +
        '/actions/runs/(?<run>[1-9][0-9]*)/job/(?<job>[1-9][0-9]*)$'
    $match = [regex]::Match($detailsUrl, $pattern)
    if (-not $match.Success) {
        throw "$description details_url is not a canonical Actions job URL for ${Repository}: $detailsUrl"
    }
    $jobId = [Int64]::Parse($match.Groups['job'].Value, [Globalization.CultureInfo]::InvariantCulture)
    if ($jobId -ne $checkRunId) {
        throw "$description details_url names job $jobId, not check run $checkRunId."
    }
    return [pscustomobject]@{
        run_id = [Int64]::Parse($match.Groups['run'].Value, [Globalization.CultureInfo]::InvariantCulture)
        job_id = $jobId
    }
}

function New-ReleasePackageDecision {
    param(
        [Parameter(Mandatory)] [string] $Repository,
        [Parameter(Mandatory)] [string] $SourceSha,
        [Parameter(Mandatory)] $Location,
        [Parameter(Mandatory)] [ValidateSet('reuse', 'rebuild')] [string] $Mode,
        [Parameter(Mandatory)] [string] $Reason,
        [AllowNull()] $Run = $null,
        [object[]] $Artifacts = @()
    )

    return [ordered]@{
        schema = 1
        repository = $Repository
        source_sha = $SourceSha
        mode = $Mode
        reason = $Reason
        build_check = [ordered]@{
            name = $script:ReleaseBuildCheckName
            run_id = $Location.run_id
            job_id = $Location.job_id
        }
        run = $Run
        artifacts = @($Artifacts)
    }
}

function Get-ReleasePackageDecision {
    param(
        [Parameter(Mandatory)] [string] $Repository,
        [Parameter(Mandatory)] [string] $SourceSha,
        [Parameter(Mandatory)] $Location,
        [Parameter(Mandatory)] $Job,
        [Parameter(Mandatory)] $Run,
        [Parameter(Mandatory)] $ArtifactList
    )

    # The job is the resolver's receipt seen from the Actions API. Anything
    # other than the same successful job on the same source contradicts it.
    $jobDescription = "Actions job $($Location.job_id)"
    if ((Get-PositiveInteger -Object $Job -Name 'id' -Description $jobDescription) -ne $Location.job_id -or
        (Get-PositiveInteger -Object $Job -Name 'run_id' -Description $jobDescription) -ne $Location.run_id) {
        throw "$jobDescription does not belong to run $($Location.run_id)."
    }
    if ((Get-ExactString -Object $Job -Name 'head_sha' -Description $jobDescription) -cne $SourceSha) {
        throw "$jobDescription did not build source $SourceSha."
    }
    if ((Get-ExactString -Object $Job -Name 'name' -Description $jobDescription) -cne $script:ReleaseBuildCheckName) {
        throw "$jobDescription is not the '$($script:ReleaseBuildCheckName)' job."
    }
    if ((Get-ExactString -Object $Job -Name 'status' -Description $jobDescription) -cne 'completed' -or
        (Get-ExactProperty -Object $Job -Name 'conclusion' -Description $jobDescription) -cne 'success') {
        throw "$jobDescription is not a completed success."
    }
    $runAttempt = Get-PositiveInteger -Object $Job -Name 'run_attempt' -Description $jobDescription

    $runDescription = "Workflow run $($Location.run_id)"
    if ((Get-PositiveInteger -Object $Run -Name 'id' -Description $runDescription) -ne $Location.run_id) {
        throw "$runDescription response names a different run."
    }
    if ((Get-ExactString -Object $Run -Name 'head_sha' -Description $runDescription) -cne $SourceSha) {
        throw "$runDescription did not build source $SourceSha."
    }
    $runRepository = Get-ExactProperty -Object $Run -Name 'repository' -Description $runDescription
    if ((Get-ExactString -Object $runRepository -Name 'full_name' -Description "$runDescription repository") -cne $Repository) {
        throw "$runDescription belongs to another repository."
    }
    $runSummary = [ordered]@{
        id = $Location.run_id
        attempt = $runAttempt
        html_url = Get-ExactString -Object $Run -Name 'html_url' -Description $runDescription
    }

    $ineligible = [System.Collections.Generic.List[string]]::new()
    $runEvent = Get-ExactString -Object $Run -Name 'event' -Description $runDescription
    $branch = Get-ExactString -Object $Run -Name 'head_branch' -Description $runDescription
    $path = Get-ExactString -Object $Run -Name 'path' -Description $runDescription
    if ($runEvent -cne 'push') {
        $ineligible.Add("$runDescription was triggered by '$runEvent', not a push.")
    }
    if ($branch -cne 'main') {
        $ineligible.Add("$runDescription ran on '$branch', not main.")
    }
    if ($path -cne $script:ReleaseBuildWorkflowPath) {
        $ineligible.Add("$runDescription ran '$path', not $($script:ReleaseBuildWorkflowPath).")
    }
    if ($ineligible.Count -gt 0) {
        return New-ReleasePackageDecision -Repository $Repository -SourceSha $SourceSha -Location $Location `
            -Mode 'rebuild' -Reason ($ineligible -join ' ') -Run $runSummary
    }

    $listDescription = "Artifact listing for run $($Location.run_id)"
    $totalCount = [Int64] (Get-ExactProperty -Object $ArtifactList -Name 'total_count' -Description $listDescription)
    $artifacts = @(Get-ExactProperty -Object $ArtifactList -Name 'artifacts' -Description $listDescription)
    if ($totalCount -ne $artifacts.Count) {
        throw "$listDescription returned $($artifacts.Count) of $totalCount artifacts."
    }
    foreach ($artifact in $artifacts) {
        $artifactDescription = "Artifact $(Get-PositiveInteger -Object $artifact -Name 'id' -Description 'Artifact')"
        $workflowRun = Get-ExactProperty -Object $artifact -Name 'workflow_run' -Description $artifactDescription
        if ((Get-PositiveInteger -Object $workflowRun -Name 'id' -Description "$artifactDescription workflow_run") -ne $Location.run_id -or
            (Get-ExactString -Object $workflowRun -Name 'head_sha' -Description "$artifactDescription workflow_run") -cne $SourceSha) {
            throw "$artifactDescription was not uploaded by run $($Location.run_id) for $SourceSha."
        }
    }

    $absent = [System.Collections.Generic.List[string]]::new()
    $selected = [System.Collections.Generic.List[object]]::new()
    foreach ($name in Get-ReusedArtifactNames) {
        $named = @($artifacts | Where-Object { (Get-ExactString -Object $_ -Name 'name' -Description 'Artifact') -ceq $name })
        $live = @($named | Where-Object { (Get-ExactProperty -Object $_ -Name 'expired' -Description "Artifact '$name'") -eq $false })
        if ($named.Count -eq 0) {
            $absent.Add("'$name' was never uploaded.")
        }
        elseif ($live.Count -eq 0) {
            $absent.Add("'$name' has expired.")
        }
        elseif ($live.Count -gt 1) {
            $absent.Add("'$name' is ambiguous: $($live.Count) unexpired copies.")
        }
        else {
            $selected.Add([ordered]@{
                name = $name
                id = Get-PositiveInteger -Object $live[0] -Name 'id' -Description "Artifact '$name'"
                digest = Get-ExactString -Object $live[0] -Name 'digest' -Description "Artifact '$name'"
                size_in_bytes = Get-PositiveInteger -Object $live[0] -Name 'size_in_bytes' -Description "Artifact '$name'"
            })
        }
    }
    if ($absent.Count -gt 0) {
        return New-ReleasePackageDecision -Repository $Repository -SourceSha $SourceSha -Location $Location `
            -Mode 'rebuild' -Reason "$runDescription cannot supply the tested package: $($absent -join ' ')" -Run $runSummary
    }

    return New-ReleasePackageDecision -Repository $Repository -SourceSha $SourceSha -Location $Location `
        -Mode 'reuse' -Reason "$runDescription attempt $runAttempt built and tested this source on main." `
        -Run $runSummary -Artifacts @($selected)
}

function Invoke-PackageSourceSelfTest {
    $repository = 'owner/repo'
    $sourceSha = 'a' * 40
    $otherSha = 'b' * 40

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

    function New-Check {
        param([string] $Name = 'MSBuild (Release, x64)', [Int64] $Id = 501, [string] $Url = $null)
        if ($null -eq $Url -or $Url -eq '') { $Url = "https://github.com/$repository/actions/runs/700/job/$Id" }
        return [pscustomobject]@{ name = $Name; check_run_id = $Id; details_url = $Url }
    }

    function New-Job {
        return [pscustomobject]@{
            id = [Int64] 501; run_id = [Int64] 700; run_attempt = 2; head_sha = $sourceSha
            name = 'MSBuild (Release, x64)'; status = 'completed'; conclusion = 'success'
        }
    }

    function New-Run {
        return [pscustomobject]@{
            id = [Int64] 700; head_sha = $sourceSha; event = 'push'; head_branch = 'main'
            path = '.github/workflows/build-sakura.yml'; html_url = "https://github.com/$repository/actions/runs/700"
            repository = [pscustomobject]@{ full_name = $repository }
        }
    }

    function New-Artifact {
        param([string] $Name, [Int64] $Id, [bool] $Expired = $false, [Int64] $RunId = 700, [string] $HeadSha = $sourceSha)
        return [pscustomobject]@{
            id = $Id; name = $Name; expired = $Expired; size_in_bytes = [Int64] 10
            digest = "sha256:$('c' * 64)"
            workflow_run = [pscustomobject]@{ id = $RunId; head_sha = $HeadSha }
        }
    }

    function New-ArtifactList {
        param([object[]] $Artifacts)
        return [pscustomobject]@{ total_count = $Artifacts.Count; artifacts = $Artifacts }
    }

    function New-CompleteArtifacts {
        $id = 900
        $list = foreach ($name in Get-ReusedArtifactNames) {
            $id++
            New-Artifact -Name $name -Id $id
        }
        return @($list)
    }

    $location = Get-ReleaseBuildCheckLocation -Repository $repository -RequiredChecks @(
        (New-Check -Name 'check-encoding' -Id 1),
        (New-Check)
    )
    Assert-True ($location.run_id -eq 700 -and $location.job_id -eq 501) 'the build check locates its run and job'

    Assert-Throws { Get-ReleaseBuildCheckLocation -Repository $repository -RequiredChecks @((New-Check -Name 'check-encoding')) } "exactly one 'MSBuild (Release, x64)' check"
    Assert-Throws { Get-ReleaseBuildCheckLocation -Repository $repository -RequiredChecks @((New-Check), (New-Check)) } "found 2"
    Assert-Throws { Get-ReleaseBuildCheckLocation -Repository $repository -RequiredChecks @((New-Check -Url "https://github.com/$repository/runs/501")) } 'canonical Actions job URL'
    Assert-Throws { Get-ReleaseBuildCheckLocation -Repository $repository -RequiredChecks @((New-Check -Url "https://github.com/other/repo/actions/runs/700/job/501")) } 'canonical Actions job URL'
    Assert-Throws { Get-ReleaseBuildCheckLocation -Repository $repository -RequiredChecks @((New-Check -Url "https://github.com/$repository/actions/runs/700/job/502")) } 'not check run 501'

    $reuse = Get-ReleasePackageDecision -Repository $repository -SourceSha $sourceSha -Location $location `
        -Job (New-Job) -Run (New-Run) -ArtifactList (New-ArtifactList (New-CompleteArtifacts))
    Assert-True ($reuse.mode -ceq 'reuse') 'a complete main-push run is reused'
    Assert-True ($reuse.run.id -eq 700 -and $reuse.run.attempt -eq 2) 'reuse names the tested run attempt'
    Assert-True (@($reuse.artifacts).Count -eq 6) 'reuse selects every required artifact'
    Assert-True ((@($reuse.artifacts | ForEach-Object { $_.name }) -join '|') -ceq ((Get-ReusedArtifactNames) -join '|')) 'reuse keeps the declared artifact order'

    $requested = New-ReleasePackageDecision -Repository $repository -SourceSha $sourceSha -Location $location -Mode 'rebuild' -Reason 'requested'
    Assert-True ($requested.mode -ceq 'rebuild' -and @($requested.artifacts).Count -eq 0) 'a requested rebuild selects no artifact'

    $preRecord = @(New-CompleteArtifacts | Where-Object { $_.name -cne 'Release build record x64 Release' })
    $decision = Get-ReleasePackageDecision -Repository $repository -SourceSha $sourceSha -Location $location `
        -Job (New-Job) -Run (New-Run) -ArtifactList (New-ArtifactList $preRecord)
    Assert-True ($decision.mode -ceq 'rebuild' -and $decision.reason.Contains('was never uploaded')) 'a source that predates the record rebuilds'

    $expired = @(New-CompleteArtifacts | ForEach-Object { if ($_.name -ceq 'Asm x64 Release') { $_.expired = $true }; $_ })
    $decision = Get-ReleasePackageDecision -Repository $repository -SourceSha $sourceSha -Location $location `
        -Job (New-Job) -Run (New-Run) -ArtifactList (New-ArtifactList $expired)
    Assert-True ($decision.mode -ceq 'rebuild' -and $decision.reason.Contains('has expired')) 'an expired archive rebuilds'

    $duplicate = @(New-CompleteArtifacts) + @(New-Artifact -Name 'Exe x64 Release' -Id 999)
    $decision = Get-ReleasePackageDecision -Repository $repository -SourceSha $sourceSha -Location $location `
        -Job (New-Job) -Run (New-Run) -ArtifactList (New-ArtifactList $duplicate)
    Assert-True ($decision.mode -ceq 'rebuild' -and $decision.reason.Contains('ambiguous')) 'duplicate unexpired archives rebuild'

    $expiredDuplicate = @(New-CompleteArtifacts) + @(New-Artifact -Name 'Exe x64 Release' -Id 999 -Expired $true)
    $decision = Get-ReleasePackageDecision -Repository $repository -SourceSha $sourceSha -Location $location `
        -Job (New-Job) -Run (New-Run) -ArtifactList (New-ArtifactList $expiredDuplicate)
    Assert-True ($decision.mode -ceq 'reuse') 'an expired earlier copy does not make the live copy ambiguous'

    foreach ($case in @(
        @{ property = 'event'; value = 'workflow_dispatch'; expected = 'not a push' },
        @{ property = 'head_branch'; value = 'feature'; expected = 'not main' },
        @{ property = 'path'; value = '.github/workflows/release-promotion.yml'; expected = 'not .github/workflows/build-sakura.yml' }
    )) {
        $run = New-Run
        $run.($case.property) = $case.value
        $decision = Get-ReleasePackageDecision -Repository $repository -SourceSha $sourceSha -Location $location `
            -Job (New-Job) -Run $run -ArtifactList (New-ArtifactList (New-CompleteArtifacts))
        Assert-True ($decision.mode -ceq 'rebuild' -and $decision.reason.Contains($case.expected)) "a run with $($case.property)=$($case.value) rebuilds"
    }

    foreach ($case in @(
        @{ property = 'head_sha'; value = $otherSha; expected = 'did not build source' },
        @{ property = 'run_id'; value = [Int64] 701; expected = 'does not belong to run 700' },
        @{ property = 'id'; value = [Int64] 502; expected = 'does not belong to run 700' },
        @{ property = 'name'; value = 'MSBuild (Debug, x64)'; expected = "is not the 'MSBuild (Release, x64)' job" },
        @{ property = 'conclusion'; value = 'failure'; expected = 'is not a completed success' },
        @{ property = 'status'; value = 'in_progress'; expected = 'is not a completed success' },
        @{ property = 'run_attempt'; value = 0; expected = 'must be a positive integer' }
    )) {
        $job = New-Job
        $job.($case.property) = $case.value
        Assert-Throws { Get-ReleasePackageDecision -Repository $repository -SourceSha $sourceSha -Location $location `
            -Job $job -Run (New-Run) -ArtifactList (New-ArtifactList (New-CompleteArtifacts)) } $case.expected
    }

    $run = New-Run
    $run.head_sha = $otherSha
    Assert-Throws { Get-ReleasePackageDecision -Repository $repository -SourceSha $sourceSha -Location $location `
        -Job (New-Job) -Run $run -ArtifactList (New-ArtifactList (New-CompleteArtifacts)) } 'did not build source'
    $run = New-Run
    $run.repository.full_name = 'other/repo'
    Assert-Throws { Get-ReleasePackageDecision -Repository $repository -SourceSha $sourceSha -Location $location `
        -Job (New-Job) -Run $run -ArtifactList (New-ArtifactList (New-CompleteArtifacts)) } 'belongs to another repository'

    $foreign = @(New-CompleteArtifacts) + @(New-Artifact -Name 'unrelated' -Id 998 -RunId 701)
    Assert-Throws { Get-ReleasePackageDecision -Repository $repository -SourceSha $sourceSha -Location $location `
        -Job (New-Job) -Run (New-Run) -ArtifactList (New-ArtifactList $foreign) } 'was not uploaded by run 700'
    $foreign = @(New-CompleteArtifacts) + @(New-Artifact -Name 'unrelated' -Id 998 -HeadSha $otherSha)
    Assert-Throws { Get-ReleasePackageDecision -Repository $repository -SourceSha $sourceSha -Location $location `
        -Job (New-Job) -Run (New-Run) -ArtifactList (New-ArtifactList $foreign) } 'was not uploaded by run 700'
    $incomplete = [pscustomobject]@{ total_count = 7; artifacts = @(New-CompleteArtifacts) }
    Assert-Throws { Get-ReleasePackageDecision -Repository $repository -SourceSha $sourceSha -Location $location `
        -Job (New-Job) -Run (New-Run) -ArtifactList $incomplete } 'returned 6 of 7 artifacts'

    Write-Host 'Select-ReleasePackageSource self-test passed.'
}

if ($SelfTest) {
    Invoke-PackageSourceSelfTest
    return
}

$sourceEvidence = Get-Content -LiteralPath $SourceEvidencePath -Raw | ConvertFrom-Json
$sourceSha = Get-ExactString -Object $sourceEvidence -Name 'source_sha' -Description 'Release source evidence'
if ($sourceSha -cnotmatch '^[0-9a-f]{40}$') {
    throw "Release source evidence names an invalid source SHA '$sourceSha'."
}
$requiredChecks = @(Get-ExactProperty -Object $sourceEvidence -Name 'required_checks' -Description 'Release source evidence')
$location = Get-ReleaseBuildCheckLocation -Repository $Repository -RequiredChecks $requiredChecks

if ($RebuildRequested -ceq 'true') {
    $decision = New-ReleasePackageDecision -Repository $Repository -SourceSha $sourceSha -Location $location `
        -Mode 'rebuild' -Reason 'A rebuild was requested for this dispatch.'
}
else {
    $apiRoot = if ([string]::IsNullOrWhiteSpace($env:GITHUB_API_URL)) { 'https://api.github.com' } else { $env:GITHUB_API_URL.TrimEnd('/') }
    $headers = @{
        Accept = 'application/vnd.github+json'
        Authorization = "Bearer $Token"
        'X-GitHub-Api-Version' = '2022-11-28'
    }
    $job = Invoke-RestMethod -Method Get -Headers $headers -Uri "$apiRoot/repos/$Repository/actions/jobs/$($location.job_id)"
    $run = Invoke-RestMethod -Method Get -Headers $headers -Uri "$apiRoot/repos/$Repository/actions/runs/$($location.run_id)"
    $artifactList = Invoke-RestMethod -Method Get -Headers $headers -Uri "$apiRoot/repos/$Repository/actions/runs/$($location.run_id)/artifacts?per_page=100"
    $decision = Get-ReleasePackageDecision -Repository $Repository -SourceSha $sourceSha -Location $location `
        -Job $job -Run $run -ArtifactList $artifactList
}

Write-Utf8NoBomFile -Path $OutputPath -Content ($decision | ConvertTo-Json -Depth 8)
Write-Host "Release package source: $($decision.mode). $($decision.reason)"
