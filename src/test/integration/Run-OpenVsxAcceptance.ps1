[CmdletBinding()]
param(
    [string]$CacheRoot = (Join-Path $env:USERPROFILE "tmp\sakura-open-vsx-acceptance")
)

$ErrorActionPreference = "Stop"
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\..\.."))
$allowedRoot = [IO.Path]::GetFullPath((Join-Path $env:USERPROFILE "tmp")).TrimEnd('\')
$cache = [IO.Path]::GetFullPath($CacheRoot).TrimEnd('\')
if (-not $cache.StartsWith("$allowedRoot\", [StringComparison]::OrdinalIgnoreCase)) {
    throw "CacheRoot must be a child of $allowedRoot"
}
New-Item -ItemType Directory -Path $cache -Force | Out-Null

$node = Get-Command node.exe -ErrorAction Stop
$sevenZip = Get-Command 7z.exe -ErrorAction SilentlyContinue
if (-not $sevenZip) {
    $fallback = "C:\Program Files\7-Zip\7z.exe"
    if (Test-Path -LiteralPath $fallback -PathType Leaf) { $sevenZip = $fallback }
}
if (-not $sevenZip) { throw "7z.exe is required to extract pinned VSIX files" }
$nodePath = if ($node.Source) { $node.Source } else { $node.Path }
$sevenZipPath = if ($sevenZip -is [string]) { $sevenZip } elseif ($sevenZip.Source) { $sevenZip.Source } else { $sevenZip.Path }
$auditScript = Join-Path $PSScriptRoot "open-vsx-acceptance.cjs"

$extensions = @(
    [pscustomobject]@{
        Slug = "editorconfig"
        ExtensionId = "editorconfig.editorconfig"
        Version = "0.18.2"
        Url = "https://open-vsx.org/api/EditorConfig/EditorConfig/0.18.2/file/EditorConfig.EditorConfig-0.18.2.vsix"
        Sha256 = "cbc0370ff204bc16d84a3ee6830536fc04355856fa0e895ab064e1a03cfcb84a"
        Expectation = "editorconfig"
    },
    [pscustomobject]@{
        Slug = "prettier"
        ExtensionId = "esbenp.prettier-vscode"
        Version = "12.4.0"
        Url = "https://open-vsx.org/api/esbenp/prettier-vscode/12.4.0/file/esbenp.prettier-vscode-12.4.0.vsix"
        Sha256 = "fb730ea4306d09cdc0a3aaa9e9baae28058cc97a4fbfce8b056b377a0639a9fe"
        Expectation = "format"
    },
    [pscustomobject]@{
        Slug = "markdownlint"
        ExtensionId = "davidanson.vscode-markdownlint"
        Version = "0.62.0"
        Url = "https://open-vsx.org/api/DavidAnson/vscode-markdownlint/0.62.0/file/DavidAnson.vscode-markdownlint-0.62.0.vsix"
        Sha256 = "7a13722513e8798909aa57363c3f3b3aa7d537895ab850c74bdf0a4d749d5d73"
        Expectation = "diagnostics"
    },
    [pscustomobject]@{
        Slug = "cspell"
        ExtensionId = "streetsidesoftware.code-spell-checker"
        Version = "4.5.6"
        Url = "https://open-vsx.org/api/streetsidesoftware/code-spell-checker/4.5.6/file/streetsidesoftware.code-spell-checker-4.5.6.vsix"
        Sha256 = "0006a465e0a13791e4861a9b1945923179b84dbabb9fec9d5aeb440cf51d46a4"
        Expectation = "diagnostics"
    }
)

function Assert-CacheChild([string]$Path) {
    $full = [IO.Path]::GetFullPath($Path)
    if (-not $full.StartsWith("$cache\", [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to mutate a path outside the acceptance cache: $full"
    }
    return $full
}

function Invoke-BoundedDownload([string]$Uri, [string]$Destination) {
    $partial = Assert-CacheChild "$Destination.part"
    for ($attempt = 1; $attempt -le 3; $attempt++) {
        try {
            Remove-Item -LiteralPath $partial -Force -ErrorAction SilentlyContinue
            Invoke-WebRequest -UseBasicParsing -Uri $Uri -OutFile $partial -TimeoutSec 120
            Move-Item -LiteralPath $partial -Destination $Destination -Force
            return
        } catch {
            Remove-Item -LiteralPath $partial -Force -ErrorAction SilentlyContinue
            if ($attempt -eq 3) { throw }
            $delay = ([Math]::Pow(2, $attempt - 1) * 250) + (Get-Random -Minimum 0 -Maximum 151)
            Start-Sleep -Milliseconds ([int]$delay)
        }
    }
}

function Get-Sha256Hex([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    try {
        $sha256 = [Security.Cryptography.SHA256]::Create()
        try {
            return ([BitConverter]::ToString($sha256.ComputeHash($stream))).Replace("-", "").ToLowerInvariant()
        } finally {
            $sha256.Dispose()
        }
    } finally {
        $stream.Dispose()
    }
}

function Get-VerifiedVsix($extension) {
    $downloadRoot = Assert-CacheChild (Join-Path $cache "downloads")
    New-Item -ItemType Directory -Path $downloadRoot -Force | Out-Null
    $vsix = Assert-CacheChild (Join-Path $downloadRoot "$($extension.Slug)-$($extension.Version).vsix")
    if (Test-Path -LiteralPath $vsix -PathType Leaf) {
        $existingHash = Get-Sha256Hex $vsix
        if ($existingHash -ne $extension.Sha256) { Remove-Item -LiteralPath $vsix -Force }
    }
    if (-not (Test-Path -LiteralPath $vsix -PathType Leaf)) {
        Invoke-BoundedDownload $extension.Url $vsix
    }
    $actualHash = Get-Sha256Hex $vsix
    if ($actualHash -ne $extension.Sha256) {
        throw "SHA-256 mismatch for $($extension.ExtensionId): expected $($extension.Sha256), got $actualHash"
    }
    return $vsix
}

function Expand-CleanVsix($extension, [string]$Vsix) {
    $extractRoot = Assert-CacheChild (Join-Path $cache "extracted\$($extension.Slug)-$($extension.Version)-$($extension.Sha256.Substring(0, 12))")
    if (Test-Path -LiteralPath $extractRoot) { Remove-Item -LiteralPath $extractRoot -Recurse -Force }
    New-Item -ItemType Directory -Path $extractRoot -Force | Out-Null
    & $sevenZipPath x -y "-o$extractRoot" $Vsix | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "7z failed for $($extension.ExtensionId) with exit code $LASTEXITCODE" }
    $extensionRoot = Join-Path $extractRoot "extension"
    if (-not (Test-Path -LiteralPath (Join-Path $extensionRoot "package.json") -PathType Leaf)) {
        throw "VSIX for $($extension.ExtensionId) does not contain extension/package.json"
    }
    return $extensionRoot
}

function Assert-AcceptanceResult($extension, $result, [string]$sampleText) {
    if ($result.extensionId -ne $extension.ExtensionId) {
        throw "Extension identity mismatch: expected $($extension.ExtensionId), got $($result.extensionId)"
    }
    if ($result.activated -ne $true) { throw "$($extension.ExtensionId) did not activate" }
    switch ($extension.Expectation) {
        "editorconfig" {
            if ($result.methods -notcontains "window/editor/setOptions") {
                throw "$($extension.ExtensionId) did not apply .editorconfig editor options"
            }
            if ($result.editorOptions.insertSpaces -ne $true -or [int]$result.editorOptions.tabSize -ne 3) {
                throw "$($extension.ExtensionId) returned unexpected editor options: $($result.editorOptions | ConvertTo-Json -Compress)"
            }
        }
        "format" {
            $edits = @($result.formatResult.value)
            if ($edits.Count -eq 0 -or @($edits | Where-Object { $_.newText -and $_.newText -ne $sampleText }).Count -eq 0) {
                throw "$($extension.ExtensionId) returned no effective formatting edit"
            }
        }
        "diagnostics" {
            if ([int]$result.diagnosticCount -le 0) {
                throw "$($extension.ExtensionId) produced no diagnostics for the known-bad sample"
            }
        }
        default { throw "Unknown acceptance expectation: $($extension.Expectation)" }
    }
}

$results = [Collections.Generic.List[object]]::new()
foreach ($extension in $extensions) {
    Write-Host "[Open VSX] $($extension.ExtensionId) $($extension.Version)"
    $vsix = Get-VerifiedVsix $extension
    $extensionRoot = Expand-CleanVsix $extension $vsix
    $profileRoot = Assert-CacheChild (Join-Path $cache "profiles\$($extension.Slug)-$([Guid]::NewGuid().ToString('N'))")
    $workspaceRoot = Assert-CacheChild (Join-Path $profileRoot "workspace")
    New-Item -ItemType Directory -Path $workspaceRoot -Force | Out-Null
    $sampleText = "#bad  `n`nmisspeled  text`nconst value={answer:42};`n"
    $samplePath = Join-Path $workspaceRoot "sample.md"
    Set-Content -LiteralPath $samplePath -Value $sampleText -Encoding UTF8 -NoNewline
    Set-Content -LiteralPath (Join-Path $workspaceRoot ".editorconfig") -Encoding ASCII -Value @(
        "root = true",
        "",
        "[*]",
        "indent_style = space",
        "indent_size = 3",
        "end_of_line = lf"
    )
    try {
        $previousErrorAction = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        try {
            $output = @(& $nodePath $auditScript $extensionRoot $samplePath $profileRoot 2>&1)
            $exitCode = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $previousErrorAction
        }
        if ($exitCode -ne 0) {
            throw "$($extension.ExtensionId) audit exited with $exitCode`n$($output -join [Environment]::NewLine)"
        }
        $jsonLine = @($output | ForEach-Object { $_.ToString() } | Where-Object { $_.TrimStart().StartsWith("{") }) | Select-Object -Last 1
        if (-not $jsonLine) { throw "$($extension.ExtensionId) produced no JSON result" }
        $result = $jsonLine | ConvertFrom-Json
        Assert-AcceptanceResult $extension $result $sampleText
        $results.Add([pscustomobject]@{
            extensionId = $result.extensionId
            version = $extension.Version
            sha256 = $extension.Sha256
            representativeResult = $extension.Expectation
            diagnosticCount = [int]$result.diagnosticCount
            methods = @($result.methods)
        })
        Write-Host "[PASS] $($extension.ExtensionId)"
    } finally {
        if (Test-Path -LiteralPath $profileRoot) { Remove-Item -LiteralPath $profileRoot -Recurse -Force }
    }
}

[pscustomobject]@{
    passed = $results.Count
    expected = $extensions.Count
    results = $results
} | ConvertTo-Json -Depth 6

if ($results.Count -ne $extensions.Count) { exit 1 }
