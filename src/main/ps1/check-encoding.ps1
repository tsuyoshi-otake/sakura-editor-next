param(
	[ValidateSet('diff', 'all')]
	[string]$Mode = 'diff',
	[string]$BaseSha = $env:CHECK_ENCODING_BASE_SHA,
	[string]$RepositoryRoot = (Get-Location).Path
)

$ErrorActionPreference = 'Stop'
$targetExtensions = @('.cpp', '.h', '.rc', '.rc2')

# Redirected output is what CI and the tests read, so it is UTF-8 whatever the
# console code page is (#326). An interactive console keeps the host's writer.
# Setting [Console]::OutputEncoding instead would change the code page of the
# console this process shares with its caller.
$utf8 = [Text.UTF8Encoding]::new($false)
if ([Console]::IsOutputRedirected) {
	$outputWriter = [IO.StreamWriter]::new([Console]::OpenStandardOutput(), $utf8)
	$outputWriter.AutoFlush = $true
}
else {
	$outputWriter = [Console]::Out
}
if ([Console]::IsErrorRedirected) {
	$errorWriter = [IO.StreamWriter]::new([Console]::OpenStandardError(), $utf8)
	$errorWriter.AutoFlush = $true
}
else {
	$errorWriter = [Console]::Error
}

function Invoke-GitText {
	param(
		[string[]]$Arguments
	)

	$startInfo = [Diagnostics.ProcessStartInfo]::new()
	$startInfo.FileName = 'git'
	$startInfo.WorkingDirectory = $RepositoryRoot
	$startInfo.UseShellExecute = $false
	$startInfo.RedirectStandardOutput = $true
	$startInfo.RedirectStandardError = $true
	# git writes paths and messages as UTF-8; the default is the console code
	# page, which turns a non-ASCII path into one that does not exist (#326).
	$startInfo.StandardOutputEncoding = $utf8
	$startInfo.StandardErrorEncoding = $utf8
	foreach ($argument in $Arguments) {
		$startInfo.ArgumentList.Add($argument)
	}

	$process = [Diagnostics.Process]::Start($startInfo)
	try {
		# Drain stderr concurrently: reading the streams one after the other
		# deadlocks once git fills the stderr pipe while stdout is still open.
		$stderrRead = $process.StandardError.ReadToEndAsync()
		$stdout = $process.StandardOutput.ReadToEnd()
		$stderr = $stderrRead.GetAwaiter().GetResult()
		$process.WaitForExit()
		if ($process.ExitCode -ne 0) {
			throw "git $($Arguments[0]) failed with exit code $($process.ExitCode): $stderr"
		}
		return $stdout
	}
	finally {
		$process.Dispose()
	}
}

function Test-TargetExtension {
	param([string]$Path)
	return $targetExtensions -contains [IO.Path]::GetExtension($Path).ToLowerInvariant()
}

function Get-DiffFiles {
	param([string]$Commit)

	if ([string]::IsNullOrWhiteSpace($Commit) -or $Commit -eq ('0' * 40)) {
		$outputWriter.WriteLine('base SHA is empty or all-zero; branch creation has no diff')
		return @()
	}

	[void](Invoke-GitText @('rev-parse', '--verify', '--end-of-options', "$Commit^{commit}"))
	$output = Invoke-GitText @(
		'diff', '--name-only', '--diff-filter=d', '-z', $Commit, 'HEAD', '--'
	)
	return @(
		$output.Split([char]0, [StringSplitOptions]::RemoveEmptyEntries) |
			Where-Object { Test-TargetExtension $_ }
	)
}

function Get-AllFiles {
	$options = [IO.EnumerationOptions]::new()
	$options.RecurseSubdirectories = $true
	$options.IgnoreInaccessible = $true
	$options.AttributesToSkip = [IO.FileAttributes]::ReparsePoint
	return @(
		[IO.Directory]::EnumerateFiles($RepositoryRoot, '*', $options) |
			Where-Object { Test-TargetExtension $_ }
	)
}

function Test-StrictDecode {
	param(
		[Text.Encoding]$Encoding,
		[byte[]]$Bytes,
		[int]$Offset
	)
	try {
		[void]$Encoding.GetString($Bytes, $Offset, $Bytes.Length - $Offset)
		return $true
	}
	catch [Text.DecoderFallbackException] {
		return $false
	}
}

function Test-SourceEncoding {
	param([byte[]]$Bytes)

	$asciiOnly = $true
	foreach ($value in $Bytes) {
		if ($value -ge 0x80) {
			$asciiOnly = $false
			break
		}
	}
	if ($asciiOnly) {
		return $true
	}
	if ($Bytes.Length -lt 3 -or
		$Bytes[0] -ne 0xEF -or $Bytes[1] -ne 0xBB -or $Bytes[2] -ne 0xBF) {
		return $false
	}
	return Test-StrictDecode ([Text.UTF8Encoding]::new($false, $true)) $Bytes 3
}

function Test-ResourceEncoding {
	param([byte[]]$Bytes)

	if ($Bytes.Length -lt 2) {
		return $false
	}
	if ($Bytes[0] -eq 0xFF -and $Bytes[1] -eq 0xFE) {
		$encoding = [Text.UnicodeEncoding]::new($false, $true, $true)
	}
	elseif ($Bytes[0] -eq 0xFE -and $Bytes[1] -eq 0xFF) {
		$encoding = [Text.UnicodeEncoding]::new($true, $true, $true)
	}
	else {
		return $false
	}
	return Test-StrictDecode $encoding $Bytes 2
}

function Test-OwnedSnapshotPath {
	param([string]$Path)

	$normalized = $Path.Replace('\', '/').ToLowerInvariant()
	return $normalized.StartsWith('third_party/owned/') -or
		$normalized.Contains('/third_party/owned/')
}

$failureCount = 0
try {
	$files = if ($Mode -eq 'all') {
		Get-AllFiles
	}
	else {
		Get-DiffFiles $BaseSha
	}
	$files = @(
		$files | Where-Object { -not (Test-OwnedSnapshotPath $_) }
	)
	foreach ($file in $files) {
		$fullPath = if ([IO.Path]::IsPathRooted($file)) { $file } else { Join-Path $RepositoryRoot $file }
		$outputWriter.WriteLine("checking $file")
		$bytes = [IO.File]::ReadAllBytes($fullPath)
		$extension = [IO.Path]::GetExtension($file).ToLowerInvariant()
		$valid = if ($extension -in @('.cpp', '.h')) {
			Test-SourceEncoding $bytes
		}
		else {
			Test-ResourceEncoding $bytes
		}
		if ($valid) {
			$outputWriter.WriteLine("OK $file")
		}
		else {
			$errorWriter.WriteLine("NG $file")
			$failureCount++
		}
	}
}
catch {
	# The checker owns this report. The host's error view would add colors,
	# echo and truncate the source line, and encode it with the console code
	# page (#326).
	foreach ($line in $_.Exception.Message.Trim().Split("`n")) {
		$errorWriter.WriteLine("check-encoding: $($line.TrimEnd("`r"))")
	}
	exit 1
}

if ($failureCount -gt 0) {
	$errorWriter.WriteLine("$failureCount file(s) have an unsupported encoding.")
	exit 1
}
$outputWriter.WriteLine("Encoding check passed for $($files.Count) file(s).")
exit 0
