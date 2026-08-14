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

# Validate the source gate contexts that actually run on a main push. The PR
# target-policy context is intentionally absent: it is a routing check on the
# pull-request merge ref and does not run again on the final main SHA.
$requiredCheckNames = @(
    'check-encoding',
    'MSBuild (Debug, x64)',
    'MSBuild (Release, x64)',
    'cppcheck (x64, Release)',
    'doxygen (x64, Release)'
)
$githubActionsAppId = 15368
$verifiedChecks = [System.Collections.Generic.List[object]]::new()
foreach ($requiredCheckName in $requiredCheckNames) {
    $matches = @($checkRunsResponse.check_runs | Where-Object {
        $_.name -eq $requiredCheckName -and [int64] $_.app.id -eq $githubActionsAppId
    })
    if ($matches.Count -eq 0) {
        throw "Expected at least one GitHub Actions check named '$requiredCheckName' for $sourceSha; found none."
    }

    # A maintainer can legitimately re-run or cancel a completed main-push
    # workflow. `filter=latest` is scoped to check suites rather than globally
    # unique names, so require one completed success for this immutable SHA.
    # A still-running retry is not stable evidence and blocks promotion; an
    # older cancelled retry does not erase the successful required gate that
    # tested the same source commit.
    $activeMatches = @($matches | Where-Object { $_.status -ne 'completed' })
    if ($activeMatches.Count -ne 0) {
        throw "Required source-gate check '$requiredCheckName' for $sourceSha still has $($activeMatches.Count) non-completed run(s)."
    }
    $successfulMatches = @($matches | Where-Object {
        $_.status -eq 'completed' -and $_.conclusion -eq 'success'
    })
    if ($successfulMatches.Count -eq 0) {
        $states = ($matches | ForEach-Object { "$($_.status)/$($_.conclusion)" }) -join ', '
        throw "Required source-gate check '$requiredCheckName' for $sourceSha has no completed success. Observed: $states"
    }
    $sortProperties = @(
        @{ Expression = { [DateTime] $_.started_at }; Descending = $true },
        @{ Expression = { [Int64] $_.id }; Descending = $true }
    )
    $check = @($successfulMatches | Sort-Object -Property $sortProperties | Select-Object -First 1)[0]

    $verifiedChecks.Add([ordered]@{
        name = $check.name
        check_run_id = [int64] $check.id
        matching_check_run_count = $matches.Count
        successful_check_run_count = $successfulMatches.Count
        app_id = [int64] $check.app.id
        status = $check.status
        conclusion = $check.conclusion
        details_url = $check.details_url
        started_at = $check.started_at
        completed_at = $check.completed_at
    })
}

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
