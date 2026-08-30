function Format-ReleasePromotionNotes {
    [CmdletBinding()]
    [OutputType([string])]
    param(
        [Parameter(Mandatory)] [string] $SourceTag,
        [Parameter(Mandatory)] [string] $SourceSha,
        [Parameter(Mandatory)] [string] $FileVersion,
        [Parameter(Mandatory)] $RequiredChecks
    )

    if ([string]::IsNullOrWhiteSpace($SourceTag) -or
        [string]::IsNullOrWhiteSpace($SourceSha) -or
        [string]::IsNullOrWhiteSpace($FileVersion)) {
        throw 'Release-note source identity contains an empty value.'
    }
    if ($null -eq $RequiredChecks -or $RequiredChecks -is [string] -or
        -not ($RequiredChecks -is [Collections.IEnumerable])) {
        throw 'Required check evidence must be a non-empty collection.'
    }

    $checkNames = [System.Collections.Generic.List[string]]::new()
    $seenCheckNames = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($check in $RequiredChecks) {
        if ($null -eq $check) {
            throw 'Required check evidence contains a null record.'
        }
        $nameProperty = $check.PSObject.Properties['name']
        if ($null -eq $nameProperty -or -not ($nameProperty.Value -is [string])) {
            throw "Required check evidence contains a record without a string 'name'."
        }
        $name = $nameProperty.Value.Trim()
        if ([string]::IsNullOrWhiteSpace($name) -or $name.Contains("`r") -or $name.Contains("`n")) {
            throw 'Required check evidence contains an empty or multiline check name.'
        }
        if (-not $seenCheckNames.Add($name)) {
            throw "Required check evidence contains duplicate check name '$name'."
        }
        [void] $checkNames.Add($name)
    }
    if ($checkNames.Count -eq 0) {
        throw 'Release source evidence did not record any required checks.'
    }

    $lines = [System.Collections.Generic.List[string]]::new()
    [void] $lines.Add('## Verified release promotion')
    [void] $lines.Add('')
    [void] $lines.Add("- Source tag: ``$($SourceTag.Trim())``")
    [void] $lines.Add("- Source SHA: ``$($SourceSha.Trim().ToLowerInvariant())``")
    [void] $lines.Add("- File version: ``$($FileVersion.Trim())``")
    [void] $lines.Add('')
    [void] $lines.Add('The source SHA passed the required main-branch checks before packaging. The packaged installer was provenance-checked, installed into an isolated directory, opened with a real document, and uninstalled before this release was published.')
    [void] $lines.Add('')
    [void] $lines.Add('Required source checks:')
    foreach ($checkName in $checkNames) {
        [void] $lines.Add("- $checkName")
    }
    return [string]::Join("`n", $lines)
}
