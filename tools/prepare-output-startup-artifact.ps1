#requires -Version 5.1
<##
.SYNOPSIS
  Builds one explicit Output backend and publishes its startup-evidence bundle.

.DESCRIPTION
  This producer owns the complete C1e artifact transaction.  It fixes the
  Output and UTF-16 selectors, invokes the canonical build and runtime-stage
  commands, captures the source/toolchain/host identities, and writes a
  schemaVersion 1 manifest beside a private copy of the canonical runtime
  stage.  The final directory is published by one directory rename only after
  every identity has been checked again.

  -SelfTest exercises the schema, closure, atomic publication, and bounded
  process ownership helpers.  It never invokes MSBuild, Cargo, Python, or the
  runtime-stage command.
##>
[CmdletBinding()]
param(
    [string]$Backend = 'cpp',
    [ValidateSet('x64')]
    [string]$Platform = 'x64',
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [Alias('OutputRoot', 'ArtifactRoot', 'EvidenceDirectory')]
    [string]$OutputDirectory = 'build/evidence/output-startup-artifacts',
    [Alias('Jobs', 'Parallelism')]
    [ValidateRange(1, 256)]
    [int]$BuildParallelism = 1,
    [Alias('BuildTimeoutSeconds')]
    [ValidateRange(1, 7200)]
    [int]$TimeoutSeconds = 1800,
    [switch]$SelfTest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:SchemaVersion = 1
$script:Stage = 'preflight'
$script:Published = $false
$script:BuildStarted = $false
$script:ProducerExitCode = 1
$script:LockPath = $null
$script:LockHandle = $null
$script:LockOwned = $false
$script:TransactionRoot = $null
$script:RepoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$script:OutputProviderSymbols = @(
    'sakura_output_provider_create_v1',
    'sakura_output_provider_apply_v1',
    'sakura_output_provider_snapshot_measure_v1',
    'sakura_output_provider_snapshot_write_v1',
    'sakura_output_provider_active_channel_v1',
    'sakura_output_provider_stop_v1',
    'sakura_output_provider_destroy_v1'
)

if (-not ('SakuraOutputStartupNative' -as [type]) -and
    [Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public sealed class SakuraOutputStartupProcessResult
{
    public bool Succeeded;
    public int ErrorCode;
    public IntPtr ProcessHandle;
    public IntPtr ThreadHandle;
    public int ProcessId;
}

public static class SakuraOutputStartupNative
{
    private const uint CREATE_SUSPENDED = 0x00000004;
    private const uint CREATE_UNICODE_ENVIRONMENT = 0x00000400;
    private const uint CREATE_NO_WINDOW = 0x08000000;
    private const uint JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x00002000;
    private const uint WAIT_OBJECT_0 = 0x00000000;
    private const uint WAIT_TIMEOUT = 0x00000102;
    private const uint STILL_ACTIVE = 259;
    private const int JobObjectExtendedLimitInformation = 9;
    private const uint STARTF_USESHOWWINDOW = 0x00000001;

    [StructLayout(LayoutKind.Sequential)]
    private struct JOBOBJECT_BASIC_LIMIT_INFORMATION
    {
        public long PerProcessUserTimeLimit;
        public long PerJobUserTimeLimit;
        public uint LimitFlags;
        public UIntPtr MinimumWorkingSetSize;
        public UIntPtr MaximumWorkingSetSize;
        public uint ActiveProcessLimit;
        public UIntPtr Affinity;
        public uint PriorityClass;
        public uint SchedulingClass;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct IO_COUNTERS
    {
        public ulong ReadOperationCount;
        public ulong WriteOperationCount;
        public ulong OtherOperationCount;
        public ulong ReadTransferCount;
        public ulong WriteTransferCount;
        public ulong OtherTransferCount;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct JOBOBJECT_EXTENDED_LIMIT_INFORMATION
    {
        public JOBOBJECT_BASIC_LIMIT_INFORMATION BasicLimitInformation;
        public IO_COUNTERS IoInfo;
        public UIntPtr ProcessMemoryLimit;
        public UIntPtr PeakProcessMemoryUsed;
        public UIntPtr JobMemoryLimit;
        public UIntPtr PeakJobMemoryUsed;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct STARTUPINFO
    {
        public int cb;
        public string lpReserved;
        public string lpDesktop;
        public string lpTitle;
        public uint dwX;
        public uint dwY;
        public uint dwXSize;
        public uint dwYSize;
        public uint dwXCountChars;
        public uint dwYCountChars;
        public uint dwFillAttribute;
        public uint dwFlags;
        public short wShowWindow;
        public short cbReserved2;
        public IntPtr lpReserved2;
        public IntPtr hStdInput;
        public IntPtr hStdOutput;
        public IntPtr hStdError;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct PROCESS_INFORMATION
    {
        public IntPtr hProcess;
        public IntPtr hThread;
        public int dwProcessId;
        public int dwThreadId;
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true, EntryPoint = "CreateJobObjectW")]
    private static extern IntPtr CreateJobObject(IntPtr attributes, string name);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool SetInformationJobObject(
        IntPtr job, int informationClass, IntPtr information, uint informationLength);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true, EntryPoint = "CreateProcessW")]
    private static extern bool CreateProcess(
        string applicationName,
        StringBuilder commandLine,
        IntPtr processAttributes,
        IntPtr threadAttributes,
        bool inheritHandles,
        uint creationFlags,
        IntPtr environment,
        string currentDirectory,
        ref STARTUPINFO startupInfo,
        out PROCESS_INFORMATION processInformation);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern uint ResumeThread(IntPtr thread);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GetExitCodeProcess(IntPtr process, out uint exitCode);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool TerminateJobObject(IntPtr job, uint exitCode);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool CloseHandle(IntPtr handle);

    public static IntPtr CreateKillOnCloseJob()
    {
        IntPtr job = CreateJobObject(IntPtr.Zero, null);
        if (job == IntPtr.Zero) return IntPtr.Zero;
        var limits = new JOBOBJECT_EXTENDED_LIMIT_INFORMATION();
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        IntPtr nativeLimits = Marshal.AllocHGlobal(Marshal.SizeOf(typeof(JOBOBJECT_EXTENDED_LIMIT_INFORMATION)));
        try
        {
            Marshal.StructureToPtr(limits, nativeLimits, false);
            if (!SetInformationJobObject(
                job,
                JobObjectExtendedLimitInformation,
                nativeLimits,
                unchecked((uint)Marshal.SizeOf(typeof(JOBOBJECT_EXTENDED_LIMIT_INFORMATION))))
            )
            {
                CloseHandle(job);
                return IntPtr.Zero;
            }
        }
        finally { Marshal.FreeHGlobal(nativeLimits); }
        return job;
    }

    public static SakuraOutputStartupProcessResult CreateSuspendedProcess(
        string applicationName,
        string commandLine,
        string workingDirectory,
        string environmentBlock)
    {
        var result = new SakuraOutputStartupProcessResult { Succeeded = false, ErrorCode = 0 };
        var startup = new STARTUPINFO();
        startup.cb = Marshal.SizeOf(typeof(STARTUPINFO));
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = 0;
        IntPtr nativeEnvironment = IntPtr.Zero;
        try
        {
            if (!String.IsNullOrEmpty(environmentBlock))
                nativeEnvironment = Marshal.StringToHGlobalUni(environmentBlock);
            var mutableCommandLine = new StringBuilder(commandLine);
            PROCESS_INFORMATION info;
            if (!CreateProcess(
                applicationName,
                mutableCommandLine,
                IntPtr.Zero,
                IntPtr.Zero,
                false,
                CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
                nativeEnvironment,
                workingDirectory,
                ref startup,
                out info))
            {
                result.ErrorCode = Marshal.GetLastWin32Error();
                return result;
            }
            result.Succeeded = true;
            result.ProcessHandle = info.hProcess;
            result.ThreadHandle = info.hThread;
            result.ProcessId = info.dwProcessId;
            return result;
        }
        finally
        {
            if (nativeEnvironment != IntPtr.Zero) Marshal.FreeHGlobal(nativeEnvironment);
        }
    }

    public static bool AssignProcess(IntPtr job, IntPtr process)
    {
        return job != IntPtr.Zero && process != IntPtr.Zero && AssignProcessToJobObject(job, process);
    }

    public static bool Resume(IntPtr thread)
    {
        return thread != IntPtr.Zero && ResumeThread(thread) != UInt32.MaxValue;
    }

    public static uint Wait(IntPtr process, uint milliseconds)
    {
        return process == IntPtr.Zero ? UInt32.MaxValue : WaitForSingleObject(process, milliseconds);
    }

    public static bool IsSignaled(uint waitResult)
    {
        return waitResult == WAIT_OBJECT_0;
    }

    public static bool IsTimeout(uint waitResult)
    {
        return waitResult == WAIT_TIMEOUT;
    }

    public static bool TryGetExitCode(IntPtr process, out uint exitCode)
    {
        exitCode = STILL_ACTIVE;
        return process != IntPtr.Zero && GetExitCodeProcess(process, out exitCode);
    }

    public static bool Terminate(IntPtr job, uint exitCode)
    {
        return job != IntPtr.Zero && TerminateJobObject(job, exitCode);
    }

    public static bool Close(IntPtr handle)
    {
        return handle == IntPtr.Zero || CloseHandle(handle);
    }
}
'@
}

function Get-PropertyValue {
    param(
        [Parameter(Mandatory = $true)] [AllowNull()] [object]$Object,
        [Parameter(Mandatory = $true)] [string[]]$Names
    )
    if ($null -eq $Object) { return $null }
    foreach ($name in $Names) {
        if ($Object -is [Collections.IDictionary] -and $Object.Contains($name)) {
            return $Object[$name]
        }
        $property = $Object.PSObject.Properties[$name]
        if ($null -ne $property) { return $property.Value }
    }
    return $null
}

function Convert-RuntimeReceiptPath {
    param(
        [Parameter(Mandatory = $true)] [AllowNull()] [object]$Value,
        [Parameter(Mandatory = $true)] [string]$FieldName
    )
    $path = [string]$Value
    if ([string]::IsNullOrWhiteSpace($path) -or $path -ne $path.Trim() -or $path.IndexOf([char]0) -ge 0) {
        throw "The runtime-stage receipt $FieldName is empty or contains unsafe whitespace."
    }
    $normalized = $path.Replace('/', '\')
    if ([IO.Path]::IsPathRooted($normalized) -or $normalized.IndexOf(':') -ge 0) {
        throw "The runtime-stage receipt $FieldName must be relative and may not contain an alternate data stream."
    }
    $parts = @($normalized -split '\\')
    if ($parts.Count -eq 0 -or @($parts | Where-Object {
            [string]::IsNullOrWhiteSpace($_) -or $_ -eq '.' -or $_ -eq '..' -or $_ -match '[\x00-\x1f<>\"|?*]' -or
            $_ -match '[ \.]$' -or $_ -match '^(?i:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\..*)?$'
        }).Count -gt 0) {
        throw "The runtime-stage receipt $FieldName contains an unsafe path component."
    }
    return ($parts -join '\')
}

function Get-RuntimeReceiptPathBinding {
    param(
        [Parameter(Mandatory = $true)] [string]$Destination,
        [Parameter(Mandatory = $true)] [string]$Source,
        [Parameter(Mandatory = $true)] [string]$Context
    )
    if ($Context -cne 'msvc-x64-debug' -and $Context -cne 'msvc-x64-release') {
        throw 'The runtime-stage receipt context must be the canonical MSVC x64 context.'
    }
    $destinationPath = Convert-RuntimeReceiptPath $Destination 'destination'
    $sourcePath = Convert-RuntimeReceiptPath $Source 'source'
    $destinationPrefix = ('build/staging/{0}/sakura-editor/' -f $Context).Replace('/', '\')
    $configuration = if ($Context -ceq 'msvc-x64-debug') { 'Debug' } else { 'Release' }
    $sourcePrefix = ('x64/{0}/' -f $configuration).Replace('/', '\')
    if (-not $destinationPath.StartsWith($destinationPrefix, [StringComparison]::Ordinal) -or
        -not $sourcePath.StartsWith($sourcePrefix, [StringComparison]::Ordinal)) {
        throw 'The runtime-stage receipt source or destination is outside its canonical prefix.'
    }
    $destinationSuffix = $destinationPath.Substring($destinationPrefix.Length)
    $sourceSuffix = $sourcePath.Substring($sourcePrefix.Length)
    if ([string]::IsNullOrWhiteSpace($destinationSuffix) -or
        -not [StringComparer]::Ordinal.Equals($destinationSuffix, $sourceSuffix)) {
        throw 'The runtime-stage receipt source and destination suffixes do not match.'
    }
    return [pscustomobject][ordered]@{
        destination = $destinationPath
        source = $sourcePath
        relativePath = $destinationSuffix
    }
}

function Assert-RuntimeReceiptArtifactIdentity {
    param(
        [Parameter(Mandatory = $true)] [string]$ArtifactId,
        [Parameter(Mandatory = $true)] [string]$Role,
        [Parameter(Mandatory = $true)] [string]$RelativePath,
        [Parameter(Mandatory = $true)] [string]$Context
    )
    if ([string]::IsNullOrWhiteSpace($ArtifactId) -or $ArtifactId -match '[\r\n]' -or
        [string]::IsNullOrWhiteSpace($Role) -or $Role -match '[\r\n]') {
        throw 'The runtime-stage receipt artifact_id and role must be non-empty identities.'
    }
    if ([StringComparer]::Ordinal.Equals($RelativePath, 'sakura.exe')) {
        if (-not [StringComparer]::Ordinal.Equals($ArtifactId, ('sakura-editor-{0}-product' -f $Context)) -or
            -not [StringComparer]::Ordinal.Equals($Role, 'editor')) {
            throw 'The runtime-stage receipt editor identity is not canonical.'
        }
        return
    }
    if ([StringComparer]::Ordinal.Equals($Role, 'editor') -or
        [StringComparer]::Ordinal.Equals($ArtifactId, ('sakura-editor-{0}-product' -f $Context))) {
        throw 'Only sakura.exe may use the runtime-stage editor identity.'
    }
    $knownLanguages = @{
        'sakura_lang_en_US.dll' = [pscustomobject]@{ artifactId = 'sakura-language-en-us-resource'; role = 'language-en-us' }
        'sakura_lang_zh_CN.dll' = [pscustomobject]@{ artifactId = 'sakura-language-zh-cn-resource'; role = 'language-zh-cn' }
    }
    if (($ArtifactId -ceq 'sakura-language-en-us-resource' -or $Role -ceq 'language-en-us') -and
        -not [StringComparer]::Ordinal.Equals($RelativePath, 'sakura_lang_en_US.dll')) {
        throw 'The runtime-stage en-US language identity must use its canonical top-level path.'
    }
    if (($ArtifactId -ceq 'sakura-language-zh-cn-resource' -or $Role -ceq 'language-zh-cn') -and
        -not [StringComparer]::Ordinal.Equals($RelativePath, 'sakura_lang_zh_CN.dll')) {
        throw 'The runtime-stage zh-CN language identity must use its canonical top-level path.'
    }
    if ($knownLanguages.ContainsKey($RelativePath)) {
        $expected = $knownLanguages[$RelativePath]
        if (-not [StringComparer]::Ordinal.Equals($ArtifactId, $expected.artifactId) -or
            -not [StringComparer]::Ordinal.Equals($Role, $expected.role)) {
            throw 'The runtime-stage receipt language identity is not canonical.'
        }
    }
}

function Test-Sha256 {
    param([AllowNull()] [object]$Value)
    return $null -ne $Value -and [string]$Value -match '^[0-9a-fA-F]{64}$'
}

function Get-Sha256 {
    param([Parameter(Mandatory = $true)] [string]$Path)
    $item = Get-Item -LiteralPath $Path -Force -ErrorAction Stop
    if ($item -isnot [IO.FileInfo] -or -not $item.Exists -or
        (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
        throw 'The hashed input must be a regular non-reparse file.'
    }
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try {
        $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
        try {
            return ([BitConverter]::ToString($algorithm.ComputeHash($stream))).Replace('-', '').ToLowerInvariant()
        }
        finally { $stream.Dispose() }
    }
    finally { $algorithm.Dispose() }
}

function Get-TextSha256 {
    param([Parameter(Mandatory = $true)] [AllowEmptyString()] [string]$Value)
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try {
        $encoding = New-Object Text.UTF8Encoding($false)
        return ([BitConverter]::ToString($algorithm.ComputeHash($encoding.GetBytes($Value)))).Replace('-', '').ToLowerInvariant()
    }
    finally { $algorithm.Dispose() }
}

function Get-CanonicalPath {
    param([Parameter(Mandatory = $true)] [string]$Path)
    return [IO.Path]::GetFullPath($Path)
}

function Test-PathBelow {
    param(
        [Parameter(Mandatory = $true)] [string]$Path,
        [Parameter(Mandatory = $true)] [string]$Root
    )
    $pathValue = (Get-CanonicalPath $Path).TrimEnd('\')
    $rootValue = (Get-CanonicalPath $Root).TrimEnd('\')
    if ([StringComparer]::OrdinalIgnoreCase.Equals($pathValue, $rootValue)) { return $true }
    return $pathValue.StartsWith($rootValue + '\', [StringComparison]::OrdinalIgnoreCase)
}

function Assert-NoReparseAncestors {
    param([Parameter(Mandatory = $true)] [string]$Path)
    $candidate = Get-CanonicalPath $Path
    while (-not [string]::IsNullOrEmpty($candidate)) {
        if (Test-Path -LiteralPath $candidate) {
            $item = Get-Item -LiteralPath $candidate -Force -ErrorAction Stop
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw 'A producer path crosses a reparse point.'
            }
        }
        $parent = Split-Path -Parent $candidate
        if ([string]::IsNullOrEmpty($parent) -or $parent -eq $candidate) { break }
        $candidate = $parent
    }
}

function Assert-RegularDirectory {
    param([Parameter(Mandatory = $true)] [string]$Path)
    $item = Get-Item -LiteralPath $Path -Force -ErrorAction Stop
    if ($item -isnot [IO.DirectoryInfo] -or
        (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
        throw 'A producer directory must be regular and non-reparse.'
    }
}

function Assert-RegularFile {
    param([Parameter(Mandatory = $true)] [string]$Path)
    $item = Get-Item -LiteralPath $Path -Force -ErrorAction Stop
    if ($item -isnot [IO.FileInfo] -or -not $item.Exists -or
        (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
        throw 'A producer file must be regular and non-reparse.'
    }
}

function Get-FileIdentity {
    param([Parameter(Mandatory = $true)] [string]$Path)
    Assert-RegularFile $Path
    $item = Get-Item -LiteralPath $Path -Force -ErrorAction Stop
    return [pscustomobject][ordered]@{
        sha256 = Get-Sha256 $Path
        sizeBytes = [UInt64]$item.Length
    }
}

function Get-OptionalFileIdentity {
    param([Parameter(Mandatory = $true)] [string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) {
        return [pscustomobject][ordered]@{ exists = $false; sha256 = $null; sizeBytes = [UInt64]0 }
    }
    $identity = Get-FileIdentity $Path
    return [pscustomobject][ordered]@{
        exists = $true
        sha256 = $identity.sha256
        sizeBytes = [UInt64]$identity.sizeBytes
    }
}

function Invoke-GitText {
    param([Parameter(Mandatory = $true)] [string[]]$Arguments)
    $oldErrorAction = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'SilentlyContinue'
        $value = (& git -C $script:RepoRoot @Arguments 2>$null | Out-String)
        $exitCode = $LASTEXITCODE
    }
    finally { $ErrorActionPreference = $oldErrorAction }
    if ($exitCode -ne 0) { throw 'Git source-state query failed.' }
    return [string]$value
}

function Get-SourceState {
    $statusText = Invoke-GitText @('status', '--porcelain=v1', '--untracked-files=all')
    $canonicalStatus = ($statusText -replace "`r`n", "`n" -replace "`r", "`n").TrimEnd("`n")
    $statusLines = @()
    if (-not [string]::IsNullOrEmpty($canonicalStatus)) {
        $statusLines = @($canonicalStatus -split "`n")
    }
    $head = (Invoke-GitText @('rev-parse', '--verify', 'HEAD')).Trim()
    if ($head -notmatch '^[0-9a-fA-F]{40}$') { throw 'Git HEAD is not a full commit identity.' }
    return [pscustomobject][ordered]@{
        head = $head.ToLowerInvariant()
        dirty = [bool]($statusLines.Count -ne 0)
        statusSha256 = Get-TextSha256 $canonicalStatus
        statusLineCount = [int]$statusLines.Count
    }
}

function Assert-SourceStateEqual {
    param(
        [Parameter(Mandatory = $true)] [object]$Expected,
        [Parameter(Mandatory = $true)] [object]$Actual
    )
    if ($Expected.head -ne $Actual.head -or
        [bool]$Expected.dirty -ne [bool]$Actual.dirty -or
        $Expected.statusSha256 -ne $Actual.statusSha256 -or
        [int]$Expected.statusLineCount -ne [int]$Actual.statusLineCount) {
        throw 'The repository source state changed during artifact production.'
    }
}

function Resolve-Executable {
    param([Parameter(Mandatory = $true)] [string]$Name)
    $command = Get-Command $Name -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $command -or [string]::IsNullOrWhiteSpace([string]$command.Source)) {
        throw "The required build tool is unavailable."
    }
    $path = Get-CanonicalPath ([string]$command.Source)
    Assert-RegularFile $path
    return $path
}

function Get-MsvcIdentity {
    $candidate = $null
    foreach ($name in @('cl.exe', 'MSBuild.exe', 'msbuild.exe')) {
        try { $candidate = Resolve-Executable $name; break } catch { }
    }
    if ($null -eq $candidate) {
        $vswhereCandidates = @()
        if ($env:ProgramFiles) { $vswhereCandidates += (Join-Path $env:ProgramFiles 'Microsoft Visual Studio/Installer/vswhere.exe') }
        if (${env:ProgramFiles(x86)}) { $vswhereCandidates += (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe') }
        $vswhereCandidates += (Join-Path $script:RepoRoot 'tools/vswhere/vswhere.exe')
        $vswhere = $vswhereCandidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
        if ($null -ne $vswhere) {
            $oldErrorAction = $ErrorActionPreference
            try {
                $ErrorActionPreference = 'SilentlyContinue'
                $line = (& $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' 2>$null | Select-Object -First 1)
                $exitCode = $LASTEXITCODE
            }
            finally { $ErrorActionPreference = $oldErrorAction }
            if ($exitCode -eq 0 -and -not [string]::IsNullOrWhiteSpace([string]$line) -and (Test-Path -LiteralPath ([string]$line) -PathType Leaf)) {
                $candidate = Get-CanonicalPath ([string]$line).Trim()
            }
        }
    }
    if ($null -eq $candidate) { throw 'MSVC toolchain identity is unavailable.' }
    $version = [Diagnostics.FileVersionInfo]::GetVersionInfo($candidate).FileVersion
    if ([string]::IsNullOrWhiteSpace($version)) { $version = [Diagnostics.FileVersionInfo]::GetVersionInfo($candidate).ProductVersion }
    if ([string]::IsNullOrWhiteSpace($version)) { throw 'MSVC toolchain version is unavailable.' }
    return ('msvc-file-version:{0}' -f $version.Trim())
}

function Get-RustToolchainIdentity {
    $rustc = Resolve-Executable 'rustc.exe'
    $oldErrorAction = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'SilentlyContinue'
        $line = (& $rustc --version 2>$null | Select-Object -First 1)
        $exitCode = $LASTEXITCODE
    }
    finally { $ErrorActionPreference = $oldErrorAction }
    if ($exitCode -ne 0 -or [string]::IsNullOrWhiteSpace([string]$line)) { throw 'Rust toolchain identity is unavailable.' }
    $identity = ([string]$line).Trim()
    if ($identity -match '[\r\n]') { throw 'Rust toolchain identity is malformed.' }
    return $identity
}

function Get-WindowsImageIdentity {
    $version = $null
    $build = $null
    $architecture = $env:PROCESSOR_ARCHITECTURE
    try {
        $os = Get-CimInstance -ClassName Win32_OperatingSystem -ErrorAction Stop | Select-Object -First 1
        $version = [string]$os.Version
        $build = [string]$os.BuildNumber
        if ([string]::IsNullOrWhiteSpace($architecture)) { $architecture = [string]$os.OSArchitecture }
    }
    catch {
        $version = [Environment]::OSVersion.Version.ToString()
        $build = [Environment]::OSVersion.Version.Build.ToString([Globalization.CultureInfo]::InvariantCulture)
    }
    if ([string]::IsNullOrWhiteSpace($version) -or $version -match '^(0+\.)') { throw 'Windows image identity is unavailable.' }
    if ([string]::IsNullOrWhiteSpace($build)) { $build = 'unknown' }
    if ([string]::IsNullOrWhiteSpace($architecture)) { $architecture = 'unknown' }
    $identity = ('windows-version:{0}|build:{1}|arch:{2}' -f $version.Trim(), $build.Trim(), $architecture.Trim())
    if ($identity -match '[\r\n]') { throw 'Windows image identity is malformed.' }
    return [pscustomobject][ordered]@{
        identity = $identity
        sha256 = Get-TextSha256 $identity
    }
}

function Get-PowerModeIdentity {
    $powercfg = Resolve-Executable 'powercfg.exe'
    $oldErrorAction = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'SilentlyContinue'
        $text = (& $powercfg /getactivescheme 2>$null | Out-String).Trim()
        $exitCode = $LASTEXITCODE
    }
    finally { $ErrorActionPreference = $oldErrorAction }
    $match = [regex]::Match($text, '(?i)[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}')
    if ($exitCode -ne 0 -or -not $match.Success) { throw 'Active power mode identity is unavailable.' }
    $identity = 'active-power-plan:{0}' -f $match.Value.ToLowerInvariant()
    return [pscustomobject][ordered]@{
        identity = $identity
        sha256 = Get-TextSha256 $identity
    }
}

function ConvertTo-WindowsCommandLineArgument {
    param([AllowEmptyString()] [string]$Value)
    if ($null -eq $Value) { $Value = '' }
    if ($Value.Length -eq 0) { return '""' }
    $requiresQuotes = $Value.IndexOfAny([char[]]" `t`n`v`"") -ge 0
    if (-not $requiresQuotes) { return $Value }
    $builder = New-Object Text.StringBuilder
    [void]$builder.Append('"')
    $slashes = 0
    for ($index = 0; $index -lt $Value.Length; $index++) {
        $character = $Value[$index]
        if ($character -eq '\') {
            $slashes++
            continue
        }
        if ($character -eq '"') {
            for ($count = 0; $count -lt (2 * $slashes + 1); $count++) { [void]$builder.Append('\') }
            [void]$builder.Append('"')
            $slashes = 0
            continue
        }
        for ($count = 0; $count -lt $slashes; $count++) { [void]$builder.Append('\') }
        $slashes = 0
        [void]$builder.Append($character)
    }
    for ($count = 0; $count -lt (2 * $slashes); $count++) { [void]$builder.Append('\') }
    [void]$builder.Append('"')
    return $builder.ToString()
}

function New-CmdCommandLine {
    param(
        [Parameter(Mandatory = $true)] [string]$CommandText,
        [Parameter(Mandatory = $true)] [string]$ComSpec
    )
    return ('{0} /d /s /c "{1}"' -f (ConvertTo-WindowsCommandLineArgument $ComSpec), $CommandText)
}

function New-EnvironmentBlock {
    param([Parameter(Mandatory = $true)] [hashtable]$Overrides)
    $values = @{}
    foreach ($entry in [Environment]::GetEnvironmentVariables().GetEnumerator()) {
        $name = [string]$entry.Key
        if ($name -notmatch '^[^=\x00]+$') { continue }
        $values[$name] = [string]$entry.Value
    }
    foreach ($name in $Overrides.Keys) {
        $values[[string]$name] = [string]$Overrides[$name]
    }
    $names = New-Object Collections.Generic.List[string]
    foreach ($name in $values.Keys) { [void]$names.Add([string]$name) }
    $names.Sort([StringComparer]::OrdinalIgnoreCase)
    $chunks = New-Object Collections.Generic.List[string]
    foreach ($name in $names) { [void]$chunks.Add(('{0}={1}' -f $name, $values[$name])) }
    return (($chunks.ToArray() -join [char]0) + [char]0 + [char]0)
}

function Invoke-OwnedCommand {
    param(
        [Parameter(Mandatory = $true)] [string]$ApplicationPath,
        [Parameter(Mandatory = $true)] [string]$CommandLine,
        [Parameter(Mandatory = $true)] [string]$WorkingDirectory,
        [Parameter(Mandatory = $true)] [string]$EnvironmentBlock,
        [Parameter(Mandatory = $true)] [int]$Timeout
    )
    if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) {
        throw 'The producer requires Windows process ownership.'
    }
    $job = [IntPtr]::Zero
    $processHandle = [IntPtr]::Zero
    $threadHandle = [IntPtr]::Zero
    $failure = $false
    try {
        $job = [SakuraOutputStartupNative]::CreateKillOnCloseJob()
        if ($job -eq [IntPtr]::Zero) { throw 'Could not create the run-owned kill-on-close Job Object.' }
        $created = [SakuraOutputStartupNative]::CreateSuspendedProcess(
            $ApplicationPath, $CommandLine, $WorkingDirectory, $EnvironmentBlock)
        if (-not $created.Succeeded -or $created.ProcessId -le 0) {
            throw 'Could not create the suspended producer child.'
        }
        $processHandle = $created.ProcessHandle
        $threadHandle = $created.ThreadHandle
        if (-not [SakuraOutputStartupNative]::AssignProcess($job, $processHandle)) {
            throw 'Could not assign the suspended producer child to its Job Object.'
        }
        if (-not [SakuraOutputStartupNative]::Resume($threadHandle)) {
            throw 'Could not resume the assigned producer child.'
        }
        $waitMilliseconds = [uint32][Math]::Min([Int64]::MaxValue, ([Int64]$Timeout * 1000))
        $waitResult = [SakuraOutputStartupNative]::Wait($processHandle, $waitMilliseconds)
        if ([SakuraOutputStartupNative]::IsTimeout($waitResult)) {
            $failure = $true
            if (-not [SakuraOutputStartupNative]::Terminate($job, 124)) {
                throw 'Timed-out producer child could not be terminated by its Job Object.'
            }
            $settled = [SakuraOutputStartupNative]::Wait($processHandle, [uint32]5000)
            if (-not [SakuraOutputStartupNative]::IsSignaled($settled)) {
                throw 'Timed-out producer Job Object did not settle within its bounded cleanup window.'
            }
            throw 'The producer child exceeded its bounded timeout.'
        }
        if (-not [SakuraOutputStartupNative]::IsSignaled($waitResult)) {
            throw 'The producer child wait did not reach a terminal state.'
        }
        [uint32]$exitCode = 1
        if (-not [SakuraOutputStartupNative]::TryGetExitCode($processHandle, [ref]$exitCode)) {
            throw 'The producer child exit code could not be read.'
        }
        if ($exitCode -ne 0) { throw 'The canonical producer child failed.' }
        return [pscustomobject][ordered]@{
            exitCode = [int]$exitCode
            timedOut = $false
            cleanupVerified = $true
        }
    }
    catch {
        if ($job -ne [IntPtr]::Zero -and $failure -eq $false) {
            try { [void][SakuraOutputStartupNative]::Terminate($job, 125) } catch { }
        }
        throw
    }
    finally {
        if ($threadHandle -ne [IntPtr]::Zero) {
            if (-not [SakuraOutputStartupNative]::Close($threadHandle)) { throw 'Producer thread handle cleanup failed.' }
        }
        if ($processHandle -ne [IntPtr]::Zero) {
            if (-not [SakuraOutputStartupNative]::Close($processHandle)) { throw 'Producer process handle cleanup failed.' }
        }
        if ($job -ne [IntPtr]::Zero) {
            if (-not [SakuraOutputStartupNative]::Close($job)) { throw 'Producer Job Object cleanup failed.' }
        }
    }
}

function Assert-BackendSelector {
    if (-not [StringComparer]::Ordinal.Equals($Backend, 'cpp') -and
        -not [StringComparer]::Ordinal.Equals($Backend, 'rust')) {
        throw 'Backend must be exactly cpp or rust.'
    }
    if (-not [StringComparer]::Ordinal.Equals($Platform, 'x64')) { throw 'Platform must be exactly x64.' }
    if (-not [StringComparer]::Ordinal.Equals($Configuration, 'Debug') -and
        -not [StringComparer]::Ordinal.Equals($Configuration, 'Release')) {
        throw 'Configuration must be exactly Debug or Release.'
    }
    if ($BuildParallelism -lt 1 -or $BuildParallelism -gt 256) { throw 'BuildParallelism is outside its bounded range.' }
    if ($TimeoutSeconds -lt 1 -or $TimeoutSeconds -gt 7200) { throw 'TimeoutSeconds is outside its bounded range.' }
}

function Get-OutputRoot {
    param([Parameter(Mandatory = $true)] [string]$Requested)
    $root = if ([IO.Path]::IsPathRooted($Requested)) { Get-CanonicalPath $Requested } else { Get-CanonicalPath (Join-Path $script:RepoRoot $Requested) }
    $buildRoot = Get-CanonicalPath (Join-Path $script:RepoRoot 'build')
    if (-not (Test-PathBelow $root $buildRoot)) { throw 'OutputDirectory must remain below the repository build directory.' }
    Assert-NoReparseAncestors $root
    return $root
}

function Get-RuntimeStageSnapshot {
    param(
        [Parameter(Mandatory = $true)] [string]$StageRoot,
        [Parameter(Mandatory = $true)] [string]$ExpectedContext,
        [Parameter(Mandatory = $true)] [object]$ExpectedArtifact
    )
    Assert-RegularDirectory $StageRoot
    $receiptPath = Join-Path $StageRoot '.sakura-runtime-stage.json'
    Assert-RegularFile $receiptPath
    try { $receipt = Get-Content -LiteralPath $receiptPath -Raw | ConvertFrom-Json -ErrorAction Stop }
    catch { throw 'The canonical runtime-stage receipt is not valid JSON.' }
    $schema = Get-PropertyValue $receipt @('schema_version', 'schemaVersion')
    if ($null -eq $schema -or [int]$schema -ne 1) { throw 'The canonical runtime-stage receipt schema is unsupported.' }
    $context = [string](Get-PropertyValue $receipt @('context_id', 'contextId'))
    if (-not [StringComparer]::Ordinal.Equals($context, $ExpectedContext)) { throw 'The runtime-stage receipt context is not exact.' }
    $files = @(Get-PropertyValue $receipt @('files'))
    if ($files.Count -eq 0) { throw 'The canonical runtime-stage receipt declares no files.' }
    $seen = @{}
    $seenNames = @{}
    $entries = New-Object Collections.Generic.List[object]
    $editorSeen = $false
    $totalSize = [UInt64]0
    foreach ($entry in $files) {
        foreach ($required in @('artifact_id', 'destination', 'role', 'source')) {
            if ($null -eq $entry.PSObject.Properties[$required]) { throw 'The canonical runtime-stage receipt has an incomplete file entry.' }
        }
        $destination = [string](Get-PropertyValue $entry @('destination'))
        $source = [string](Get-PropertyValue $entry @('source'))
        $binding = Get-RuntimeReceiptPathBinding $destination $source $context
        $name = $binding.relativePath
        $artifactId = [string](Get-PropertyValue $entry @('artifact_id'))
        $role = [string](Get-PropertyValue $entry @('role'))
        Assert-RuntimeReceiptArtifactIdentity $artifactId $role $name $context
        if ($seen.ContainsKey($binding.destination.ToLowerInvariant())) { throw 'The canonical runtime-stage receipt has a duplicate file identity.' }
        $seen[$binding.destination.ToLowerInvariant()] = $true
        if ($seenNames.ContainsKey($name.ToLowerInvariant())) { throw 'The canonical runtime-stage receipt has an ambiguous staged file name.' }
        $seenNames[$name.ToLowerInvariant()] = $true
        $declaredHash = ([string](Get-PropertyValue $entry @('sha256', 'hash'))) -replace '^(?i:sha256:)', ''
        if (-not (Test-Sha256 $declaredHash)) { throw 'The canonical runtime-stage receipt has an invalid file hash.' }
        $declaredSizeValue = Get-PropertyValue $entry @('size', 'sizeBytes')
        try { [UInt64]$declaredSize = $declaredSizeValue } catch { throw 'The canonical runtime-stage receipt has an invalid file size.' }
        if ($declaredSize -lt 1) { throw 'The canonical runtime-stage receipt has an invalid file size.' }
        $candidate = Join-Path $StageRoot $name
        $identity = Get-FileIdentity $candidate
        if ($identity.sha256 -ne $declaredHash.ToLowerInvariant() -or
            [UInt64]$identity.sizeBytes -ne $declaredSize) {
            throw 'A canonical runtime-stage file does not match its receipt.'
        }
        [void]$entries.Add([pscustomobject][ordered]@{
            name = $name
            canonicalRelativePath = $binding.destination
            artifactId = $artifactId
            source = $binding.source
            destination = $binding.destination
            role = $role
            sha256 = $declaredHash.ToLowerInvariant()
            sizeBytes = $declaredSize
        })
        $totalSize += $declaredSize
        if ([StringComparer]::OrdinalIgnoreCase.Equals($name, 'sakura.exe') -or
            [StringComparer]::OrdinalIgnoreCase.Equals($role, 'editor')) {
            if (-not [StringComparer]::OrdinalIgnoreCase.Equals($name, 'sakura.exe')) { throw 'The runtime-stage editor entry is not sakura.exe.' }
            if ($identity.sha256 -ne [string]$ExpectedArtifact.sha256.ToLowerInvariant()) { throw 'The runtime-stage executable does not match the selected artifact.' }
            $editorSeen = $true
        }
    }
    if (-not $editorSeen) { throw 'The canonical runtime-stage receipt has no sakura.exe editor entry.' }
    foreach ($actual in [IO.Directory]::EnumerateFileSystemEntries((Get-CanonicalPath $StageRoot))) {
        $actualItem = Get-Item -LiteralPath $actual -Force -ErrorAction Stop
        if (($actualItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'The runtime stage contains a reparse point.' }
        $actualName = [IO.Path]::GetFileName($actual)
        if (-not [StringComparer]::OrdinalIgnoreCase.Equals($actualName, '.sakura-runtime-stage.json') -and
            -not $seenNames.ContainsKey($actualName.ToLowerInvariant())) {
            throw 'The runtime stage contains an undeclared file.'
        }
        if ($actualItem -is [IO.DirectoryInfo]) { throw 'The runtime stage contains an undeclared directory.' }
    }
    $canonicalLines = New-Object Collections.Generic.List[string]
    foreach ($entry in $entries) {
        [void]$canonicalLines.Add(('{0}|{1}|{2}|{3}' -f
            $entry.destination.Replace('/', '\').ToLowerInvariant(),
            $entry.role.ToLowerInvariant(),
            $entry.sha256,
            ([UInt64]$entry.sizeBytes).ToString([Globalization.CultureInfo]::InvariantCulture)))
    }
    $canonicalLines.Sort([StringComparer]::Ordinal)
    return [pscustomobject][ordered]@{
        receiptSha256 = Get-Sha256 $receiptPath
        dependencyClosureSha256 = Get-TextSha256 ($canonicalLines.ToArray() -join "`n")
        fileCount = [int]$files.Count
        totalSizeBytes = [UInt64]$totalSize
        context = $context
        receiptPath = $receiptPath
        entries = $entries.ToArray()
    }
}

function Copy-RuntimeStage {
    param(
        [Parameter(Mandatory = $true)] [string]$SourceRoot,
        [Parameter(Mandatory = $true)] [string]$DestinationRoot,
        [Parameter(Mandatory = $true)] [object]$Snapshot
    )
    if (Test-Path -LiteralPath $DestinationRoot) { throw 'The transaction runtime-stage directory already exists.' }
    Assert-NoReparseAncestors $DestinationRoot
    [void][IO.Directory]::CreateDirectory($DestinationRoot)
    foreach ($entry in @($Snapshot.entries)) {
        $source = Join-Path $SourceRoot $entry.name
        $destination = Join-Path $DestinationRoot $entry.name
        [IO.File]::Copy($source, $destination, $false)
    }
    [IO.File]::Copy((Join-Path $SourceRoot '.sakura-runtime-stage.json'), (Join-Path $DestinationRoot '.sakura-runtime-stage.json'), $false)
    return $true
}

function Write-JsonAtomic {
    param(
        [Parameter(Mandatory = $true)] [string]$Path,
        [Parameter(Mandatory = $true)] [object]$Value
    )
    $json = $Value | ConvertTo-Json -Depth 20
    $temporary = '{0}.{1}.tmp' -f $Path, ([Guid]::NewGuid().ToString('N'))
    try {
        [IO.File]::WriteAllText($temporary, $json, (New-Object Text.UTF8Encoding($false)))
        [IO.File]::Move($temporary, $Path)
    }
    finally {
        if (Test-Path -LiteralPath $temporary) { [IO.File]::Delete($temporary) }
    }
}

function Assert-PayloadFreeManifest {
    param([Parameter(Mandatory = $true)] [object]$Manifest)
    $json = $Manifest | ConvertTo-Json -Depth 20 -Compress
    if ($json -match '(?i)"(?:path|commandLine|arguments|stdout|stderr|caption|text|document|profile|exception|message|detail)"\s*:') {
        throw 'The build manifest contains a payload-bearing property.'
    }
    if ($json -match '(?i)[A-Za-z]:\\\\|\\\\\\\\') { throw 'The build manifest contains a path-shaped value.' }
    return $true
}

function New-SelectorProof {
    param(
        [Parameter(Mandatory = $true)] [object]$ProviderObjectBefore,
        [Parameter(Mandatory = $true)] [object]$ProviderObjectAfter,
        [string[]]$UnresolvedProviderSymbols = @(),
        [ValidateSet('environment-selector-verified', 'dumpbin-unresolved-refs-verified')]
        [string]$ProofResult = 'environment-selector-verified'
    )
    if ($null -eq $ProviderObjectAfter -or -not (Test-Sha256 $ProviderObjectAfter.sha256) -or
        [UInt64]$ProviderObjectAfter.sizeBytes -lt 1) {
        throw 'The selected build did not produce the Output provider object proof.'
    }
    if ($ProviderObjectBefore.exists -and -not (Test-Sha256 $ProviderObjectBefore.sha256)) {
        throw 'The pre-build Output provider object identity is invalid.'
    }
    $normalizedSymbols = @($UnresolvedProviderSymbols | ForEach-Object { ([string]$_).ToLowerInvariant() } | Sort-Object -Unique)
    $canonical = 'output={0}|utf16=cpp|output-production=false|utf16-production=false|telemetry=false|listings=false|result={1}|symbols={2}|object-after={3}' -f
        $Backend, $ProofResult, ($normalizedSymbols -join ','), ([string]$ProviderObjectAfter.sha256).ToLowerInvariant()
    return [ordered]@{
        result = $ProofResult
        outputBackend = $Backend
        utf16Backend = 'cpp'
        outputProductionPackage = $false
        utf16ProductionPackage = $false
        utf16BenchmarkTelemetry = $false
        assemblyListings = $false
        providerObjectSha256Before = if ($ProviderObjectBefore.exists) { [string]$ProviderObjectBefore.sha256 } else { $null }
        providerObjectSha256After = [string]$ProviderObjectAfter.sha256
        providerObjectSizeBytesAfter = [UInt64]$ProviderObjectAfter.sizeBytes
        unresolvedProviderSymbols = $normalizedSymbols
        unresolvedProviderSymbolCount = [int]$normalizedSymbols.Count
        selectorContractSha256 = Get-TextSha256 $canonical
    }
}

function Resolve-Dumpbin {
    param(
        [Parameter(Mandatory = $true)] [string]$ComSpec,
        [Parameter(Mandatory = $true)] [string]$WorkingDirectory,
        [Parameter(Mandatory = $true)] [string]$EnvironmentBlock,
        [Parameter(Mandatory = $true)] [int]$Timeout,
        [Parameter(Mandatory = $true)] [string]$ProbePath
    )
    try { return Resolve-Executable 'dumpbin.exe' } catch { }
    $candidateDirectories = New-Object Collections.Generic.List[string]
    foreach ($name in @('cl.exe', 'MSBuild.exe', 'msbuild.exe')) {
        try {
            $tool = Resolve-Executable $name
            $directory = Split-Path -Parent $tool
            if (-not $candidateDirectories.Contains($directory)) { [void]$candidateDirectories.Add($directory) }
            $folder = $directory
            for ($index = 0; $index -lt 3; $index++) {
                $folder = Split-Path -Parent $folder
                if ([string]::IsNullOrWhiteSpace($folder)) { break }
                [void]$candidateDirectories.Add($folder)
            }
        }
        catch { }
    }
    if ($env:VSINSTALLDIR) { [void]$candidateDirectories.Add($env:VSINSTALLDIR) }
    $vswhereCandidates = @()
    if ($env:ProgramFiles) { $vswhereCandidates += (Join-Path $env:ProgramFiles 'Microsoft Visual Studio/Installer/vswhere.exe') }
    if (${env:ProgramFiles(x86)}) { $vswhereCandidates += (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe') }
    $vswhereCandidates += (Join-Path $script:RepoRoot 'tools/vswhere/vswhere.exe')
    foreach ($programFiles in @($env:ProgramFiles, ${env:ProgramFiles(x86)})) {
        if ([string]::IsNullOrWhiteSpace([string]$programFiles)) { continue }
        $visualStudioRoot = Join-Path $programFiles 'Microsoft Visual Studio'
        if (Test-Path -LiteralPath $visualStudioRoot -PathType Container) {
            [void]$candidateDirectories.Add($visualStudioRoot)
        }
    }
    foreach ($vswhere in $vswhereCandidates) {
        if (Test-Path -LiteralPath $vswhere -PathType Leaf) {
            $vswherePath = Get-CanonicalPath $vswhere
            $errorPath = '{0}.stderr' -f $ProbePath
            try {
                $findPattern = 'VC\Tools\MSVC\**\bin\Hostx64\x64\dumpbin.exe'
                $commandText = '{0} -latest -products * -requires Microsoft.Component.MSBuild -find {1} > {2} 2> {3}' -f
                    (ConvertTo-WindowsCommandLineArgument $vswherePath),
                    (ConvertTo-WindowsCommandLineArgument $findPattern),
                    (ConvertTo-WindowsCommandLineArgument $ProbePath),
                    (ConvertTo-WindowsCommandLineArgument $errorPath)
                [void](Invoke-OwnedCommand $ComSpec (New-CmdCommandLine $commandText $ComSpec) $WorkingDirectory $EnvironmentBlock $Timeout)
                Assert-RegularFile $ProbePath
                $line = @(Get-Content -LiteralPath $ProbePath | ForEach-Object { ([string]$_).Trim() } | Where-Object { $_ }) | Select-Object -First 1
                if (-not [string]::IsNullOrWhiteSpace([string]$line)) {
                    $resolved = Get-CanonicalPath ([string]$line)
                    Assert-RegularFile $resolved
                    if ([IO.Path]::GetFileName($resolved) -ine 'dumpbin.exe') { throw 'vswhere returned a non-dumpbin path.' }
                    return $resolved
                }
            }
            finally {
                foreach ($temporary in @($ProbePath, $errorPath)) {
                    if (Test-Path -LiteralPath $temporary) {
                        try { [IO.File]::Delete($temporary) } catch { }
                    }
                }
            }
        }
    }
    $knownVisualStudioRoots = @($candidateDirectories.ToArray() | Where-Object { $_ -like '*Microsoft Visual Studio*' })
    foreach ($visualStudioRoot in $knownVisualStudioRoots) {
        foreach ($yearDirectory in @([IO.Directory]::EnumerateDirectories($visualStudioRoot))) {
            foreach ($editionDirectory in @([IO.Directory]::EnumerateDirectories($yearDirectory))) {
                [void]$candidateDirectories.Add($editionDirectory)
            }
        }
    }
    foreach ($base in $candidateDirectories) {
        if ([string]::IsNullOrWhiteSpace($base) -or -not (Test-Path -LiteralPath $base -PathType Container)) { continue }
        $candidate = Join-Path $base 'dumpbin.exe'
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { return Get-CanonicalPath $candidate }
        $vcRoot = Join-Path $base 'VC/Tools/MSVC'
        if (Test-Path -LiteralPath $vcRoot -PathType Container) {
            foreach ($versionDirectory in @([IO.Directory]::EnumerateDirectories($vcRoot) | Sort-Object -Descending)) {
                foreach ($relative in @('bin/Hostx64/x64/dumpbin.exe', 'bin/Hostx86/x64/dumpbin.exe', 'bin/Hostx64/x86/dumpbin.exe')) {
                    $candidate = Join-Path $versionDirectory $relative
                    if (Test-Path -LiteralPath $candidate -PathType Leaf) { return Get-CanonicalPath $candidate }
                }
            }
        }
    }
    throw 'MSVC dumpbin.exe is unavailable for the selector proof.'
}

function Get-SelectorProof {
    param(
        [Parameter(Mandatory = $true)] [object]$ProviderObjectBefore,
        [Parameter(Mandatory = $true)] [object]$ProviderObjectAfter,
        [Parameter(Mandatory = $true)] [string]$ObjectPath,
        [Parameter(Mandatory = $true)] [string]$DumpbinPath,
        [Parameter(Mandatory = $true)] [string]$ComSpec,
        [Parameter(Mandatory = $true)] [string]$EnvironmentBlock,
        [Parameter(Mandatory = $true)] [string]$WorkingDirectory,
        [Parameter(Mandatory = $true)] [string]$OutputPath,
        [Parameter(Mandatory = $true)] [int]$Timeout
    )
    if (-not (Test-Sha256 $ProviderObjectAfter.sha256)) { throw 'The selector proof object hash is invalid.' }
    $errorPath = '{0}.stderr' -f $OutputPath
    try {
        $commandText = '{0} /symbols {1} > {2} 2> {3}' -f
            (ConvertTo-WindowsCommandLineArgument $DumpbinPath),
            (ConvertTo-WindowsCommandLineArgument $ObjectPath),
            (ConvertTo-WindowsCommandLineArgument $OutputPath),
            (ConvertTo-WindowsCommandLineArgument $errorPath)
        [void](Invoke-OwnedCommand $ComSpec (New-CmdCommandLine $commandText $ComSpec) $WorkingDirectory $EnvironmentBlock $Timeout)
        Assert-RegularFile $OutputPath
        $symbols = New-Object Collections.Generic.List[string]
        foreach ($line in @(Get-Content -LiteralPath $OutputPath)) {
            $match = [regex]::Match([string]$line, '(?i)\bUNDEF\b.*\|\s*(sakura_output_provider_[A-Za-z0-9_]+)\s*$')
            if ($match.Success) {
                $symbol = $match.Groups[1].Value.ToLowerInvariant()
                if (-not $symbols.Contains($symbol)) { [void]$symbols.Add($symbol) }
            }
        }
        $symbols.Sort([StringComparer]::Ordinal)
        $expected = New-Object Collections.Generic.List[string]
        foreach ($expectedSymbol in $script:OutputProviderSymbols) {
            [void]$expected.Add(([string]$expectedSymbol).ToLowerInvariant())
        }
        $expected.Sort([StringComparer]::Ordinal)
        if ($Backend -eq 'rust') {
            if (($symbols.ToArray() -join '|') -cne ($expected.ToArray() -join '|')) {
                throw 'The Rust Output provider object does not reference the complete fixed v1 entrypoint set.'
            }
        }
        elseif ($symbols.Count -ne 0) {
            throw 'The C++ Output provider object unexpectedly references Rust Output entrypoints.'
        }
        return New-SelectorProof $ProviderObjectBefore $ProviderObjectAfter $symbols.ToArray() 'dumpbin-unresolved-refs-verified'
    }
    finally {
        foreach ($temporary in @($OutputPath, $errorPath)) {
            if (Test-Path -LiteralPath $temporary) {
                try { [IO.File]::Delete($temporary) } catch { }
            }
        }
    }
}

function Get-ProviderArchiveProof {
    param(
        [Parameter(Mandatory = $true)] [string]$ArchivePath,
        [Parameter(Mandatory = $true)] [string]$DumpbinPath,
        [Parameter(Mandatory = $true)] [string]$ComSpec,
        [Parameter(Mandatory = $true)] [string]$EnvironmentBlock,
        [Parameter(Mandatory = $true)] [string]$WorkingDirectory,
        [Parameter(Mandatory = $true)] [string]$OutputPath,
        [Parameter(Mandatory = $true)] [int]$Timeout
    )
    $archive = Get-FileIdentity $ArchivePath
    $errorPath = '{0}.stderr' -f $OutputPath
    try {
        $commandText = '{0} /symbols {1} > {2} 2> {3}' -f
            (ConvertTo-WindowsCommandLineArgument $DumpbinPath),
            (ConvertTo-WindowsCommandLineArgument $ArchivePath),
            (ConvertTo-WindowsCommandLineArgument $OutputPath),
            (ConvertTo-WindowsCommandLineArgument $errorPath)
        [void](Invoke-OwnedCommand $ComSpec (New-CmdCommandLine $commandText $ComSpec) $WorkingDirectory $EnvironmentBlock $Timeout)
        Assert-RegularFile $OutputPath
        $symbols = New-Object Collections.Generic.List[string]
        foreach ($line in @(Get-Content -LiteralPath $OutputPath)) {
            $match = [regex]::Match([string]$line, '(?i)^\s*[0-9A-F]+\s+[0-9A-F]+\s+SECT[0-9A-F]+\s+notype\s+\(\)\s+External\s+\|\s+(sakura_output_provider_[A-Za-z0-9_]+)\s*$')
            if ($match.Success) {
                $symbol = $match.Groups[1].Value.ToLowerInvariant()
                if (-not $symbols.Contains($symbol)) { [void]$symbols.Add($symbol) }
            }
        }
        $symbols.Sort([StringComparer]::Ordinal)
        $expected = New-Object Collections.Generic.List[string]
        foreach ($expectedSymbol in $script:OutputProviderSymbols) {
            [void]$expected.Add(([string]$expectedSymbol).ToLowerInvariant())
        }
        $expected.Sort([StringComparer]::Ordinal)
        if (($symbols.ToArray() -join '|') -cne ($expected.ToArray() -join '|')) {
            throw 'The Rust native archive does not define exactly the fixed Output provider v1 entrypoint set.'
        }
        return [ordered]@{
            result = 'dumpbin-defined-exports-verified'
            rustArchiveSha256 = [string]$archive.sha256
            rustArchiveSizeBytes = [UInt64]$archive.sizeBytes
            definedProviderSymbols = $symbols.ToArray()
            definedProviderSymbolCount = [int]$symbols.Count
        }
    }
    finally {
        foreach ($temporary in @($OutputPath, $errorPath)) {
            if (Test-Path -LiteralPath $temporary) {
                try { [IO.File]::Delete($temporary) } catch { }
            }
        }
    }
}

function Add-ProviderArchiveProof {
    param(
        [Parameter(Mandatory = $true)] [object]$SelectorProof,
        [Parameter(Mandatory = $true)] [object]$ArchiveProof
    )
    $symbols = @($ArchiveProof.definedProviderSymbols | ForEach-Object { ([string]$_).ToLowerInvariant() } | Sort-Object -Unique)
    $expected = @($script:OutputProviderSymbols | ForEach-Object { ([string]$_).ToLowerInvariant() } | Sort-Object -Unique)
    if ([string]$ArchiveProof.result -cne 'dumpbin-defined-exports-verified' -or
        -not (Test-Sha256 $ArchiveProof.rustArchiveSha256) -or
        [UInt64]$ArchiveProof.rustArchiveSizeBytes -lt 1 -or
        ($symbols -join '|') -cne ($expected -join '|')) {
        throw 'The Rust native archive proof is incomplete.'
    }
    $SelectorProof['rustArchiveResult'] = [string]$ArchiveProof.result
    $SelectorProof['rustArchiveSha256'] = [string]$ArchiveProof.rustArchiveSha256
    $SelectorProof['rustArchiveSizeBytes'] = [UInt64]$ArchiveProof.rustArchiveSizeBytes
    $SelectorProof['definedProviderSymbols'] = $symbols
    $SelectorProof['definedProviderSymbolCount'] = [int]$symbols.Count
    $SelectorProof['selectorContractSha256'] = Get-TextSha256 ('{0}|archive-result={1}|archive={2}|defined={3}' -f
        $SelectorProof.selectorContractSha256, $ArchiveProof.result, $ArchiveProof.rustArchiveSha256, ($symbols -join ','))
    return $SelectorProof
}

function New-BuildManifest {
    param(
        [Parameter(Mandatory = $true)] [object]$Source,
        [Parameter(Mandatory = $true)] [object]$ArtifactBefore,
        [Parameter(Mandatory = $true)] [object]$ArtifactAfter,
        [Parameter(Mandatory = $true)] [object]$RuntimeStage,
        [Parameter(Mandatory = $true)] [string]$WindowsImage,
        [Parameter(Mandatory = $true)] [string]$WindowsImageSha256,
        [Parameter(Mandatory = $true)] [string]$PowerMode,
        [Parameter(Mandatory = $true)] [string]$PowerModeSha256,
        [Parameter(Mandatory = $true)] [string]$MsvcIdentity,
        [Parameter(Mandatory = $true)] [string]$RustToolchain,
        [Parameter(Mandatory = $true)] [string]$RustLockSha256,
        [Parameter(Mandatory = $true)] [string]$PackagePlanSha256,
        [Parameter(Mandatory = $true)] [string]$BuildCommandSha256,
        [Parameter(Mandatory = $true)] [string]$PackagePlanCommandSha256,
        [Parameter(Mandatory = $true)] [string]$RuntimeStageCommandSha256,
        [Parameter(Mandatory = $true)] [object]$SelectorProof
    )
    return [ordered]@{
        schemaVersion = $script:SchemaVersion
        record = 'output-startup-build-manifest'
        payloadFree = $true
        status = 'committed'
        backend = $Backend
        platform = $Platform.ToLowerInvariant()
        configuration = $Configuration
        sourceHead = [string]$Source.head
        sourceDirty = [bool]$Source.dirty
        sourceStatusSha256 = [string]$Source.statusSha256
        sourceStatusLineCount = [int]$Source.statusLineCount
        outputBackend = $Backend
        utf16Backend = 'cpp'
        outputProductionPackage = $false
        utf16ProductionPackage = $false
        exeSha256 = [string]$ArtifactAfter.sha256
        artifactSha256 = [string]$ArtifactAfter.sha256
        artifactSha256Before = if ($ArtifactBefore.exists) { [string]$ArtifactBefore.sha256 } else { $null }
        artifactSha256After = [string]$ArtifactAfter.sha256
        artifactHashBefore = if ($ArtifactBefore.exists) { [string]$ArtifactBefore.sha256 } else { $null }
        artifactHashAfter = [string]$ArtifactAfter.sha256
        artifactSizeBefore = [UInt64]$ArtifactBefore.sizeBytes
        artifactSizeAfter = [UInt64]$ArtifactAfter.sizeBytes
        artifactSizeBytesBefore = [UInt64]$ArtifactBefore.sizeBytes
        artifactSizeBytesAfter = [UInt64]$ArtifactAfter.sizeBytes
        dependencyClosureSha256 = [string]$RuntimeStage.dependencyClosureSha256
        runtimeStageReceiptSha256 = [string]$RuntimeStage.receiptSha256
        runtimeStageSchemaVersion = 1
        runtimeStageFileCount = [int]$RuntimeStage.fileCount
        windowsImageIdentity = $WindowsImage
        windowsImageSha256 = $WindowsImageSha256
        powerMode = $PowerMode
        powerModeSha256 = $PowerModeSha256
        buildParallelism = [int]$BuildParallelism
        parallelism = [int]$BuildParallelism
        msvcIdentity = $MsvcIdentity
        rustToolchain = $RustToolchain
        rustLockSha256 = $RustLockSha256
        packagePlanSha256 = $PackagePlanSha256
        packagePlanHash = $PackagePlanSha256
        buildCommandSha256 = $BuildCommandSha256
        packagePlanCommandSha256 = $PackagePlanCommandSha256
        runtimeStageCommandSha256 = $RuntimeStageCommandSha256
        selectorProof = $SelectorProof
        selectorProofSha256 = [string]$SelectorProof.selectorContractSha256
        providerObjectSha256Before = $SelectorProof.providerObjectSha256Before
        providerObjectSha256After = [string]$SelectorProof.providerObjectSha256After
        canonicalRuntimeStage = $true
        transaction = [ordered]@{
            status = 'committed'
            artifactBeforeVerified = $true
            artifactAfterVerified = $true
            runtimeStageVerified = $true
            manifestGeneratedByProducer = $true
            publication = 'atomic-directory-rename'
        }
    }
}

function Assert-BuildManifest {
    param(
        [Parameter(Mandatory = $true)] [string]$Path,
        [Parameter(Mandatory = $true)] [object]$ExpectedSource,
        [Parameter(Mandatory = $true)] [object]$ExpectedArtifact,
        [Parameter(Mandatory = $true)] [object]$ExpectedStage,
        [Parameter(Mandatory = $true)] [object]$ExpectedSelectorProof
    )
    Assert-RegularFile $Path
    try { $manifest = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json -ErrorAction Stop }
    catch { throw 'The generated build manifest is not valid JSON.' }
    if ([int](Get-PropertyValue $manifest @('schemaVersion', 'schema_version')) -ne $script:SchemaVersion) { throw 'The generated build manifest schema is unsupported.' }
    $payloadFree = Get-PropertyValue $manifest @('payloadFree')
    if ($payloadFree -isnot [bool] -or -not [bool]$payloadFree) { throw 'The generated build manifest is not payload-free.' }
    if ([string](Get-PropertyValue $manifest @('status')) -cne 'committed') { throw 'The generated build manifest is not committed.' }
    foreach ($field in @('backend', 'platform', 'configuration', 'sourceHead', 'sourceStatusSha256', 'outputBackend', 'utf16Backend', 'exeSha256', 'dependencyClosureSha256', 'runtimeStageReceiptSha256', 'windowsImageIdentity', 'windowsImageSha256', 'powerMode', 'msvcIdentity', 'rustToolchain', 'rustLockSha256', 'packagePlanSha256', 'buildCommandSha256')) {
        $value = Get-PropertyValue $manifest @($field)
        if ($null -eq $value -or [string]::IsNullOrWhiteSpace([string]$value)) { throw "The generated build manifest is missing $field." }
    }
    $outputProduction = Get-PropertyValue $manifest @('outputProductionPackage')
    $utf16Production = Get-PropertyValue $manifest @('utf16ProductionPackage')
    if ($outputProduction -isnot [bool] -or $utf16Production -isnot [bool] -or
        [string]$manifest.backend -cne $Backend -or [string]$manifest.outputBackend -cne $Backend -or
        [string]$manifest.utf16Backend -cne 'cpp' -or [bool]$outputProduction -or [bool]$utf16Production) {
        throw 'The generated build manifest selectors are not exact.'
    }
    if ([string]$manifest.sourceHead -ne [string]$ExpectedSource.head -or
        [bool]$manifest.sourceDirty -ne [bool]$ExpectedSource.dirty -or
        [string]$manifest.sourceStatusSha256 -ne [string]$ExpectedSource.statusSha256) {
        throw 'The generated build manifest source identity is stale.'
    }
    $manifestArtifactHash = [string](Get-PropertyValue $manifest @('exeSha256', 'artifactSha256'))
    $manifestArtifactAfter = [string](Get-PropertyValue $manifest @('artifactSha256After', 'artifactHashAfter'))
    $manifestArtifactBefore = Get-PropertyValue $manifest @('artifactSha256Before', 'artifactHashBefore')
    if (-not (Test-Sha256 $manifestArtifactHash) -or -not (Test-Sha256 $manifestArtifactAfter) -or
        $manifestArtifactHash -ne [string]$ExpectedArtifact.sha256 -or
        $manifestArtifactAfter -ne [string]$ExpectedArtifact.sha256 -or
        ($null -ne $manifestArtifactBefore -and -not (Test-Sha256 $manifestArtifactBefore)) -or
        [string]$manifest.runtimeStageReceiptSha256 -ne [string]$ExpectedStage.receiptSha256 -or
        [string]$manifest.dependencyClosureSha256 -ne [string]$ExpectedStage.dependencyClosureSha256) {
        throw 'The generated build manifest artifact or stage identity is stale.'
    }
    $beforeSize = Get-PropertyValue $manifest @('artifactSizeBytesBefore', 'artifactSizeBefore')
    $afterSize = Get-PropertyValue $manifest @('artifactSizeBytesAfter', 'artifactSizeAfter')
    try { [UInt64]$beforeSizeValue = $beforeSize; [UInt64]$afterSizeValue = $afterSize }
    catch { throw 'The generated build manifest artifact sizes are invalid.' }
    if ($afterSizeValue -lt 1) { throw 'The generated build manifest artifact-after size is invalid.' }
    $parallelismValue = Get-PropertyValue $manifest @('buildParallelism', 'parallelism')
    if ($null -eq $parallelismValue -or [string]$parallelismValue -notmatch '^[0-9]+$' -or
        [int]$parallelismValue -lt 1 -or [int]$parallelismValue -gt 256) {
        throw 'The generated build manifest parallelism is invalid.'
    }
    $transaction = Get-PropertyValue $manifest @('transaction')
    if ($null -eq $transaction -or [string](Get-PropertyValue $transaction @('status')) -cne 'committed' -or
        (Get-PropertyValue $transaction @('artifactBeforeVerified')) -isnot [bool] -or
        -not [bool](Get-PropertyValue $transaction @('artifactBeforeVerified')) -or
        (Get-PropertyValue $transaction @('artifactAfterVerified')) -isnot [bool] -or
        -not [bool](Get-PropertyValue $transaction @('artifactAfterVerified')) -or
        (Get-PropertyValue $transaction @('runtimeStageVerified')) -isnot [bool] -or
        -not [bool](Get-PropertyValue $transaction @('runtimeStageVerified')) -or
        (Get-PropertyValue $transaction @('manifestGeneratedByProducer')) -isnot [bool] -or
        -not [bool](Get-PropertyValue $transaction @('manifestGeneratedByProducer'))) {
        throw 'The generated build manifest transaction proof is incomplete.'
    }
    $selectorProof = Get-PropertyValue $manifest @('selectorProof')
    $expectedProofResult = [string](Get-PropertyValue $ExpectedSelectorProof @('result'))
    $actualProofSymbols = @((Get-PropertyValue $selectorProof @('unresolvedProviderSymbols')) | ForEach-Object { ([string]$_).ToLowerInvariant() } | Sort-Object -Unique)
    $expectedProofSymbols = @((Get-PropertyValue $ExpectedSelectorProof @('unresolvedProviderSymbols')) | ForEach-Object { ([string]$_).ToLowerInvariant() } | Sort-Object -Unique)
    $actualDefinedSymbols = @((Get-PropertyValue $selectorProof @('definedProviderSymbols')) | ForEach-Object { ([string]$_).ToLowerInvariant() } | Sort-Object -Unique)
    $expectedDefinedSymbols = @((Get-PropertyValue $ExpectedSelectorProof @('definedProviderSymbols')) | ForEach-Object { ([string]$_).ToLowerInvariant() } | Sort-Object -Unique)
    $canonicalDefinedSymbols = @($script:OutputProviderSymbols | ForEach-Object { ([string]$_).ToLowerInvariant() } | Sort-Object -Unique)
    if ($null -eq $selectorProof -or
        [string](Get-PropertyValue $selectorProof @('result')) -cne $expectedProofResult -or
        [string](Get-PropertyValue $selectorProof @('outputBackend')) -cne $Backend -or
        [string](Get-PropertyValue $selectorProof @('utf16Backend')) -cne 'cpp' -or
        (Get-PropertyValue $selectorProof @('outputProductionPackage')) -isnot [bool] -or
        [bool](Get-PropertyValue $selectorProof @('outputProductionPackage')) -or
        (Get-PropertyValue $selectorProof @('utf16ProductionPackage')) -isnot [bool] -or
        [bool](Get-PropertyValue $selectorProof @('utf16ProductionPackage')) -or
        (Get-PropertyValue $selectorProof @('utf16BenchmarkTelemetry')) -isnot [bool] -or
        [bool](Get-PropertyValue $selectorProof @('utf16BenchmarkTelemetry')) -or
        (Get-PropertyValue $selectorProof @('assemblyListings')) -isnot [bool] -or
        [bool](Get-PropertyValue $selectorProof @('assemblyListings')) -or
        [string](Get-PropertyValue $selectorProof @('selectorContractSha256')) -ne [string]$ExpectedSelectorProof.selectorContractSha256 -or
        [string](Get-PropertyValue $selectorProof @('providerObjectSha256After')) -ne [string]$ExpectedSelectorProof.providerObjectSha256After -or
        [string](Get-PropertyValue $selectorProof @('rustArchiveResult')) -cne 'dumpbin-defined-exports-verified' -or
        -not (Test-Sha256 (Get-PropertyValue $selectorProof @('rustArchiveSha256'))) -or
        [string](Get-PropertyValue $selectorProof @('rustArchiveSha256')) -ne [string]$ExpectedSelectorProof.rustArchiveSha256 -or
        [UInt64](Get-PropertyValue $selectorProof @('rustArchiveSizeBytes')) -lt 1 -or
        [int](Get-PropertyValue $selectorProof @('definedProviderSymbolCount')) -ne $canonicalDefinedSymbols.Count -or
        (@($actualProofSymbols) -join '|') -cne (@($expectedProofSymbols) -join '|') -or
        (@($actualDefinedSymbols) -join '|') -cne (@($expectedDefinedSymbols) -join '|') -or
        (@($actualDefinedSymbols) -join '|') -cne (@($canonicalDefinedSymbols) -join '|')) {
        throw 'The generated build manifest selector proof is stale or incomplete.'
    }
    $selectorHash = Get-PropertyValue $manifest @('selectorProofSha256')
    if (-not (Test-Sha256 $selectorHash) -or [string]$selectorHash -ne [string]$ExpectedSelectorProof.selectorContractSha256) {
        throw 'The generated build manifest selector proof hash is invalid.'
    }
    [void](Assert-PayloadFreeManifest $manifest)
    return $manifest
}

function Remove-OwnedDirectory {
    param([AllowNull()] [string]$Path)
    if ([string]::IsNullOrWhiteSpace($Path) -or -not (Test-Path -LiteralPath $Path)) { return $true }
    $item = Get-Item -LiteralPath $Path -Force -ErrorAction Stop
    if ($item -isnot [IO.DirectoryInfo] -or (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
        throw 'Refusing to remove a non-owned or reparse producer directory.'
    }
    [IO.Directory]::Delete((Get-CanonicalPath $Path), $true)
    if (Test-Path -LiteralPath $Path) { throw 'Owned producer cleanup did not complete.' }
    return $true
}

function Acquire-ExclusiveLock {
    param([Parameter(Mandatory = $true)] [string]$Path)
    Assert-NoReparseAncestors $Path
    $parent = Split-Path -Parent $Path
    Assert-RegularDirectory $parent
    $stream = $null
    try {
        $stream = [IO.FileStream]::new($Path, [IO.FileMode]::CreateNew, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None, 4096, [IO.FileOptions]::DeleteOnClose)
    }
    catch {
        throw 'Another producer owns the selected backend/configuration lock.'
    }
    $script:LockPath = $Path
    $script:LockHandle = $stream
    $script:LockOwned = $true
    return $true
}

function Release-ExclusiveLock {
    if (-not $script:LockOwned) { return $true }
    if ($null -ne $script:LockHandle) {
        $script:LockHandle.Dispose()
        $script:LockHandle = $null
    }
    if ($null -ne $script:LockPath -and (Test-Path -LiteralPath $script:LockPath)) {
        throw 'Owned producer lock cleanup did not complete.'
    }
    $script:LockOwned = $false
    return $true
}

function New-FailureEnvelope {
    param(
        [Parameter(Mandatory = $true)] [string]$Type,
        [AllowNull()] [string]$PrimaryStage = $null,
        [AllowNull()] [string]$PrimaryType = $null,
        [AllowNull()] [object]$Cleanup = $null
    )
    $failure = [ordered]@{ stage = $script:Stage; type = $Type }
    if ($Type -eq 'cleanup-unverified') {
        $failure.primaryStage = if ($null -ne $PrimaryStage) { $PrimaryStage } else { $script:Stage }
        $failure.primaryType = if ($null -ne $PrimaryType) { $PrimaryType } else { 'unknown' }
    }
    $envelope = [ordered]@{
        schemaVersion = $script:SchemaVersion
        record = 'output-startup-build-manifest'
        payloadFree = $true
        status = 'failed'
        failure = $failure
        backend = $Backend
        platform = $Platform
        configuration = $Configuration
        outputBackend = $Backend
        utf16Backend = 'cpp'
        outputProductionPackage = $false
        utf16ProductionPackage = $false
        transaction = [ordered]@{ status = 'not-published'; published = $false }
    }
    if ($null -ne $Cleanup) { $envelope.cleanup = $Cleanup }
    return $envelope
}

function Invoke-SelfTest {
    Assert-BackendSelector
    $root = Join-Path $script:RepoRoot ('build/tmp/.output-startup-selftest-{0}' -f ([Guid]::NewGuid().ToString('N')))
    try {
        Assert-NoReparseAncestors $root
        [void][IO.Directory]::CreateDirectory($root)
        $exclusiveLockVerified = $false
        $selfTestLockPath = Join-Path $root 'exclusive.lock'
        [void](Acquire-ExclusiveLock $selfTestLockPath)
        $secondAcquireFailed = $false
        try { [void](Acquire-ExclusiveLock $selfTestLockPath) }
        catch { $secondAcquireFailed = $true }
        if (-not $secondAcquireFailed) { throw 'Self-test accepted a second owner for the exclusive lock.' }
        [void](Release-ExclusiveLock)
        [void](Acquire-ExclusiveLock $selfTestLockPath)
        [void](Release-ExclusiveLock)
        if (Test-Path -LiteralPath $selfTestLockPath) { throw 'Self-test exclusive lock was not deleted by its owner.' }
        $exclusiveLockVerified = $true
        $source = Join-Path $root 'sakura.exe'
        [IO.File]::WriteAllBytes($source, [byte[]](1, 2, 3, 5, 8))
        $artifact = Get-FileIdentity $source
        $receipt = Join-Path $root '.sakura-runtime-stage.json'
        $entryHash = Get-Sha256 $source
        $receiptValue = [ordered]@{
            schema_version = 1
            context_id = 'msvc-x64-debug'
            staging_set_id = 'selftest-runtime-stage'
            files = @([ordered]@{ artifact_id = 'sakura-editor-msvc-x64-debug-product'; destination = 'build/staging/msvc-x64-debug/sakura-editor/sakura.exe'; role = 'editor'; source = 'x64/Debug/sakura.exe'; sha256 = ('sha256:' + $entryHash); size = 5 })
        }
        Write-JsonAtomic $receipt $receiptValue
        $stage = Get-RuntimeStageSnapshot $root 'msvc-x64-debug' $artifact
        $transaction = Join-Path $root 'transaction'
        [void][IO.Directory]::CreateDirectory($transaction)
        $transactionStage = Join-Path $transaction 'runtime-stage'
        [void](Copy-RuntimeStage $root $transactionStage $stage)
        $copiedStage = Get-RuntimeStageSnapshot $transactionStage 'msvc-x64-debug' $artifact
        if ($copiedStage.receiptSha256 -ne $stage.receiptSha256 -or $copiedStage.dependencyClosureSha256 -ne $stage.dependencyClosureSha256) { throw 'Self-test runtime closure identity failed.' }
        foreach ($unsafePath in @('foo \bar', 'foo.', 'NUL.dll', 'COM1.txt', 'LPT9')) {
            $unsafePathRejected = $false
            try { [void](Convert-RuntimeReceiptPath $unsafePath 'self-test') } catch { $unsafePathRejected = $true }
            if (-not $unsafePathRejected) { throw 'Unsafe Windows producer runtime receipt path self-test was accepted.' }
        }
        $nestedLanguageRejected = $false
        try {
            Assert-RuntimeReceiptArtifactIdentity 'sakura-language-en-us-resource' 'language-en-us' 'nested\sakura_lang_en_US.dll' 'msvc-x64-debug'
        }
        catch { $nestedLanguageRejected = $true }
        if (-not $nestedLanguageRejected) { throw 'Nested known producer language runtime receipt identity self-test was accepted.' }
        $sourceState = [pscustomobject][ordered]@{ head = ('0' * 40); dirty = $false; statusSha256 = ('1' * 64); statusLineCount = 0 }
        $syntheticArchiveProof = [ordered]@{
            result = 'dumpbin-defined-exports-verified'
            rustArchiveSha256 = ('9' * 64)
            rustArchiveSizeBytes = [UInt64]1
            definedProviderSymbols = $script:OutputProviderSymbols
            definedProviderSymbolCount = [int]$script:OutputProviderSymbols.Count
        }
        $archiveExactSetRejected = $false
        $syntheticExtraArchiveProof = [ordered]@{
            result = 'dumpbin-defined-exports-verified'
            rustArchiveSha256 = ('9' * 64)
            rustArchiveSizeBytes = [UInt64]1
            definedProviderSymbols = @($script:OutputProviderSymbols) + @('sakura_output_provider_future_v1')
            definedProviderSymbolCount = [int]$script:OutputProviderSymbols.Count + 1
        }
        try {
            $unusedSelectorProof = New-SelectorProof ([pscustomobject]@{ exists = $false; sha256 = $null; sizeBytes = [UInt64]0 }) $artifact
            [void](Add-ProviderArchiveProof $unusedSelectorProof $syntheticExtraArchiveProof)
        }
        catch { $archiveExactSetRejected = $true }
        if (-not $archiveExactSetRejected) { throw 'Self-test accepted an extra Rust Output provider archive export.' }
        $selectorProof = New-SelectorProof ([pscustomobject]@{ exists = $false; sha256 = $null; sizeBytes = [UInt64]0 }) $artifact
        $selectorProof = Add-ProviderArchiveProof $selectorProof $syntheticArchiveProof
        $manifest = New-BuildManifest $sourceState ([pscustomobject]@{ exists = $false; sha256 = $null; sizeBytes = [UInt64]0 }) $artifact $stage 'windows-selftest' ('2' * 64) 'active-power-plan:selftest' ('3' * 64) 'msvc-selftest' 'rust-selftest' ('4' * 64) ('5' * 64) ('6' * 64) ('7' * 64) ('8' * 64) $selectorProof
        $manifestPath = Join-Path $transaction 'build-manifest.json'
        Write-JsonAtomic $manifestPath $manifest
        [void](Assert-BuildManifest $manifestPath $sourceState $artifact $stage $selectorProof)
        if ((Get-TextSha256 '') -ne 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855') { throw 'Self-test SHA-256 identity failed.' }
        $rustSelectorProofVerified = $false
        $savedBackend = $Backend
        try {
            $Backend = 'rust'
            $unorderedRustSymbols = @(
                $script:OutputProviderSymbols[6],
                $script:OutputProviderSymbols[1],
                $script:OutputProviderSymbols[4],
                $script:OutputProviderSymbols[0],
                $script:OutputProviderSymbols[5],
                $script:OutputProviderSymbols[3],
                $script:OutputProviderSymbols[2]
            )
            $rustProof = New-SelectorProof ([pscustomobject]@{ exists = $false; sha256 = $null; sizeBytes = [UInt64]0 }) $artifact $unorderedRustSymbols 'dumpbin-unresolved-refs-verified'
            $rustProof = Add-ProviderArchiveProof $rustProof $syntheticArchiveProof
            $expectedRustSymbols = @($unorderedRustSymbols | ForEach-Object { ([string]$_).ToLowerInvariant() } | Sort-Object)
            if ((@($rustProof.unresolvedProviderSymbols) -join '|') -cne ($expectedRustSymbols -join '|') -or
                [string]$rustProof.result -cne 'dumpbin-unresolved-refs-verified' -or
                [string]$rustProof.rustArchiveResult -cne 'dumpbin-defined-exports-verified' -or
                (@($rustProof.definedProviderSymbols) -join '|') -cne ($expectedRustSymbols -join '|')) {
                throw 'Self-test Rust selector proof normalization failed.'
            }
            $rustSelectorProofVerified = $true
        }
        finally { $Backend = $savedBackend }
        $cleanupEnvelopeVerified = $false
        $syntheticCleanup = [ordered]@{
            attempted = $true
            verified = $false
            finalRootExists = $true
            finalRootExpected = $false
            transactionRootExists = $false
            lockExists = $true
            remainingCount = 2
            failureCount = 1
        }
        $cleanupEnvelope = New-FailureEnvelope 'cleanup-unverified' 'self-test' 'synthetic-primary' $syntheticCleanup
        if ([string](Get-PropertyValue $cleanupEnvelope @('status')) -cne 'failed' -or
            [string](Get-PropertyValue (Get-PropertyValue $cleanupEnvelope @('failure')) @('type')) -cne 'cleanup-unverified' -or
            [string](Get-PropertyValue (Get-PropertyValue $cleanupEnvelope @('failure')) @('primaryType')) -cne 'synthetic-primary' -or
            [int](Get-PropertyValue (Get-PropertyValue $cleanupEnvelope @('cleanup')) @('remainingCount')) -ne 2) {
            throw 'Self-test cleanup failure envelope semantics failed.'
        }
        [void](Assert-PayloadFreeManifest $cleanupEnvelope)
        $cleanupEnvelopeVerified = $true
        $jobVerified = $false
        if ([Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT) {
            $comspec = Resolve-Executable 'cmd.exe'
            $overrides = @{ SAKURA_OUTPUT_BACKEND = 'cpp'; SAKURA_UTF16_BACKEND = 'cpp'; SAKURA_OUTPUT_PRODUCTION_PACKAGE = 'false'; SAKURA_UTF16_PRODUCTION_PACKAGE = 'false'; SKIP_CREATE_GITHASH = '1'; SAKURA_BUILD_JOBS = '1'; MSBUILDDISABLENODEREUSE = '1' }
            $block = New-EnvironmentBlock $overrides
            [void](Invoke-OwnedCommand $comspec (New-CmdCommandLine 'exit 0' $comspec) $script:RepoRoot $block 30)
            $jobVerified = $true
        }
        $summary = [ordered]@{
            schemaVersion = $script:SchemaVersion
            record = 'output-startup-build-producer-selftest'
            payloadFree = $true
            passed = $true
            noBuildLaunched = $true
            selectorVerified = $true
            sourceFingerprintVerified = $true
            selectorProofVerified = $true
            rustSelectorProofVerified = $rustSelectorProofVerified
            archiveExportsVerified = $true
            archiveExactSetRejected = $archiveExactSetRejected
            cleanupEnvelopeVerified = $cleanupEnvelopeVerified
            exclusiveLockVerified = $exclusiveLockVerified
            runtimeStageVerified = $true
            transactionVerified = $true
            manifestSchemaVerified = $true
            manifestPayloadFreeVerified = $true
            boundedProcessOwnershipVerified = $jobVerified
            canonicalClosureVerified = $true
        }
        Write-Output ('SELFTEST_JSON ' + ($summary | ConvertTo-Json -Compress -Depth 10))
    }
    finally {
        if ($script:LockOwned) { try { [void](Release-ExclusiveLock) } catch { } }
        [void](Remove-OwnedDirectory $root)
    }
}

function Invoke-Producer {
    $transactionRoot = $null
    $finalRoot = $null
    $published = $false
    $movedFinalRoot = $false
    $failureType = $null
    $failureStage = $null
    $successSummary = $null
    $cleanupVerified = $true
    $cleanupFailureCount = 0
    try {
        $script:Stage = 'preflight'
        Assert-BackendSelector
        if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) { throw 'The producer requires Windows.' }
        $outputRoot = Get-OutputRoot $OutputDirectory
        $configurationRoot = Join-Path $outputRoot $Configuration
        Assert-NoReparseAncestors $configurationRoot
        [void][IO.Directory]::CreateDirectory($configurationRoot)
        Assert-RegularDirectory $configurationRoot
        $finalRoot = Join-Path $configurationRoot $Backend
        if (Test-Path -LiteralPath $finalRoot) { throw 'The selected output transaction already exists; refusing overwrite.' }
        $lockRoot = Join-Path $outputRoot '.locks'
        Assert-NoReparseAncestors $lockRoot
        [void][IO.Directory]::CreateDirectory($lockRoot)
        Assert-RegularDirectory $lockRoot
        $script:LockPath = Join-Path $lockRoot ('{0}-{1}.lock' -f $Configuration.ToLowerInvariant(), $Backend)
        [void](Acquire-ExclusiveLock $script:LockPath)
        $transactionRoot = Join-Path $configurationRoot ('.{0}-transaction-{1}' -f $Backend, ([Guid]::NewGuid().ToString('N')))
        $script:TransactionRoot = $transactionRoot
        [void][IO.Directory]::CreateDirectory($transactionRoot)
        Assert-RegularDirectory $transactionRoot
        $context = 'msvc-x64-{0}' -f $Configuration.ToLowerInvariant()
        $artifactSource = Join-Path $script:RepoRoot ('x64/{0}/sakura.exe' -f $Configuration)
        $providerObjectSource = Join-Path $script:RepoRoot ('build/{0}/{1}/sakura_core/OutputServiceRustProvider.obj' -f $Platform, $Configuration)
        $rustProfile = if ($Configuration -eq 'Debug') { 'debug' } else { 'release' }
        $rustArchiveSource = Join-Path $script:RepoRoot ('build/{0}/{1}/rust/native/x86_64-pc-windows-msvc/{2}/sakura_native_ffi.lib' -f $Platform, $Configuration, $rustProfile)
        $canonicalStage = Join-Path $script:RepoRoot ('build/staging/{0}/sakura-editor' -f $context)
        $buildBatch = Join-Path $script:RepoRoot 'build-dev.bat'
        $buildScript = Join-Path $script:RepoRoot 'tools/build/sakura_build.py'
        Assert-RegularFile $buildBatch
        Assert-RegularFile $buildScript
        $script:Stage = 'source-state'
        $sourceBefore = Get-SourceState
        $artifactBefore = Get-OptionalFileIdentity $artifactSource
        $providerObjectBefore = Get-OptionalFileIdentity $providerObjectSource
        $windowsBefore = Get-WindowsImageIdentity
        $powerBefore = Get-PowerModeIdentity
        $msvcIdentity = Get-MsvcIdentity
        $rustToolchain = Get-RustToolchainIdentity
        $lockPath = Join-Path $script:RepoRoot 'rust/native/Cargo.lock'
        $rustLockSha256 = Get-Sha256 $lockPath
        $comspec = Resolve-Executable 'cmd.exe'
        $py = Resolve-Executable 'py.exe'
        $environmentOverrides = @{
            SAKURA_OUTPUT_BACKEND = $Backend
            SAKURA_UTF16_BACKEND = 'cpp'
            SAKURA_OUTPUT_PRODUCTION_PACKAGE = 'false'
            SAKURA_UTF16_PRODUCTION_PACKAGE = 'false'
            SAKURA_UTF16_BENCHMARK_TELEMETRY = 'false'
            SAKURA_GENERATE_ASSEMBLY_LISTINGS = 'false'
            SKIP_CREATE_GITHASH = '1'
            SAKURA_BUILD_JOBS = [string]$BuildParallelism
            MSBUILDDISABLENODEREUSE = '1'
            VSLANG = '1033'
        }
        $environmentBlock = New-EnvironmentBlock $environmentOverrides
        $dumpbinProbePath = Join-Path $transactionRoot 'dumpbin-path.txt'
        $dumpbin = Resolve-Dumpbin $comspec $script:RepoRoot $environmentBlock $TimeoutSeconds $dumpbinProbePath
        $packagePlanCommandText = '{0} -3 {1} --format json package plan sakura_app --context {2}' -f
            (ConvertTo-WindowsCommandLineArgument $py),
            (ConvertTo-WindowsCommandLineArgument $buildScript),
            $context
        $packagePlanPath = Join-Path $transactionRoot 'package-plan.json'
        $script:Stage = 'package-plan'
        $packagePlanCommand = New-CmdCommandLine ($packagePlanCommandText + ' > ' + (ConvertTo-WindowsCommandLineArgument $packagePlanPath) + ' 2> ' + (ConvertTo-WindowsCommandLineArgument (Join-Path $transactionRoot 'package-plan.stderr'))) $comspec
        [void](Invoke-OwnedCommand $comspec $packagePlanCommand $script:RepoRoot $environmentBlock $TimeoutSeconds)
        Assert-RegularFile $packagePlanPath
        try { $packagePlan = Get-Content -LiteralPath $packagePlanPath -Raw | ConvertFrom-Json -ErrorAction Stop } catch { throw 'Canonical package-plan output is not valid JSON.' }
        $packagePlanValue = [string](Get-PropertyValue $packagePlan @('plan_hash', 'planHash'))
        $packagePlanValue = $packagePlanValue -replace '^(?i:sha256:)', ''
        if (-not (Test-Sha256 $packagePlanValue)) { throw 'Canonical package-plan output has no valid plan hash.' }
        if ($null -eq (Get-PropertyValue $packagePlan @('required')) -or -not [bool](Get-PropertyValue $packagePlan @('required'))) { throw 'Canonical package plan did not require the declared closure.' }
        [IO.File]::Delete($packagePlanPath)
        $packagePlanStderr = Join-Path $transactionRoot 'package-plan.stderr'
        if (Test-Path -LiteralPath $packagePlanStderr) {
            Assert-RegularFile $packagePlanStderr
            [IO.File]::Delete($packagePlanStderr)
        }
        $packagePlanCommandSha256 = Get-TextSha256 ('package-plan|{0}|{1}|{2}' -f $Backend, $Platform.ToLowerInvariant(), $Configuration)
        $buildCommandSha256 = Get-TextSha256 ('build-dev.bat|x64|{0}|SAKURA_OUTPUT_BACKEND={1}|SAKURA_UTF16_BACKEND=cpp|SAKURA_OUTPUT_PRODUCTION_PACKAGE=false|SAKURA_UTF16_PRODUCTION_PACKAGE=false|SAKURA_UTF16_BENCHMARK_TELEMETRY=false|SAKURA_GENERATE_ASSEMBLY_LISTINGS=false|SKIP_CREATE_GITHASH=1|SAKURA_BUILD_JOBS={2}|MSBUILDDISABLENODEREUSE=1' -f $Configuration, $Backend, $BuildParallelism)
        $runtimeStageCommandSha256 = Get-TextSha256 ('stage-runtime|{0}|sakura_app|canonical' -f $context)
        $script:Stage = 'build'
        $script:BuildStarted = $true
        $buildCommandText = '{0} x64 {1}' -f (ConvertTo-WindowsCommandLineArgument $buildBatch), $Configuration
        [void](Invoke-OwnedCommand $comspec (New-CmdCommandLine $buildCommandText $comspec) $script:RepoRoot $environmentBlock $TimeoutSeconds)
        $sourceAfterBuild = Get-SourceState
        Assert-SourceStateEqual $sourceBefore $sourceAfterBuild
        $artifactAfter = Get-FileIdentity $artifactSource
        $providerObjectAfter = Get-FileIdentity $providerObjectSource
        $archiveProofOutput = Join-Path $transactionRoot 'archive-proof.txt'
        $selectorProofOutput = Join-Path $transactionRoot 'selector-proof.txt'
        $script:Stage = 'selector-proof'
        $archiveProof = Get-ProviderArchiveProof $rustArchiveSource $dumpbin $comspec $environmentBlock $script:RepoRoot $archiveProofOutput $TimeoutSeconds
        $selectorProof = Get-SelectorProof $providerObjectBefore $providerObjectAfter $providerObjectSource $dumpbin $comspec $environmentBlock $script:RepoRoot $selectorProofOutput $TimeoutSeconds
        $selectorProof = Add-ProviderArchiveProof $selectorProof $archiveProof
        if ([string]$selectorProof.result -cne 'dumpbin-unresolved-refs-verified') { throw 'The Output selector proof did not come from dumpbin unresolved-reference verification.' }
        if ([UInt64]$artifactAfter.sizeBytes -lt 1) { throw 'The built sakura.exe is empty.' }
        $script:Stage = 'runtime-stage'
        $stageCommandText = '{0} -3 {1} --format json stage runtime --context {2} --product sakura_app' -f
            (ConvertTo-WindowsCommandLineArgument $py),
            (ConvertTo-WindowsCommandLineArgument $buildScript),
            $context
        [void](Invoke-OwnedCommand $comspec (New-CmdCommandLine $stageCommandText $comspec) $script:RepoRoot $environmentBlock $TimeoutSeconds)
        $sourceAfterStage = Get-SourceState
        Assert-SourceStateEqual $sourceBefore $sourceAfterStage
        $stageSnapshot = Get-RuntimeStageSnapshot $canonicalStage $context $artifactAfter
        $transactionStage = Join-Path $transactionRoot 'runtime-stage'
        [void](Copy-RuntimeStage $canonicalStage $transactionStage $stageSnapshot)
        $copiedStage = Get-RuntimeStageSnapshot $transactionStage $context $artifactAfter
        if ($copiedStage.receiptSha256 -ne $stageSnapshot.receiptSha256 -or
            $copiedStage.dependencyClosureSha256 -ne $stageSnapshot.dependencyClosureSha256) {
            throw 'The transaction runtime-stage copy changed its canonical identity.'
        }
        $windowsAfter = Get-WindowsImageIdentity
        $powerAfter = Get-PowerModeIdentity
        if ($windowsAfter.sha256 -ne $windowsBefore.sha256 -or $powerAfter.sha256 -ne $powerBefore.sha256) { throw 'Host or power identity changed during production.' }
        $manifest = New-BuildManifest $sourceBefore $artifactBefore $artifactAfter $copiedStage $windowsBefore.identity $windowsBefore.sha256 $powerBefore.identity $powerBefore.sha256 $msvcIdentity $rustToolchain $rustLockSha256 $packagePlanValue $buildCommandSha256 $packagePlanCommandSha256 $runtimeStageCommandSha256 $selectorProof
        $manifestPath = Join-Path $transactionRoot 'build-manifest.json'
        $script:Stage = 'manifest'
        Write-JsonAtomic $manifestPath $manifest
        [void](Assert-BuildManifest $manifestPath $sourceBefore $artifactAfter $copiedStage $selectorProof)
        if ((Get-Sha256 $artifactSource) -ne [string]$artifactAfter.sha256) { throw 'The selected artifact changed after manifest generation.' }
        if (Test-Path -LiteralPath $finalRoot) { throw 'The selected output transaction appeared during production.' }
        $script:Stage = 'publication'
        [IO.Directory]::Move($transactionRoot, $finalRoot)
        $movedFinalRoot = $true
        $finalManifest = Join-Path $finalRoot 'build-manifest.json'
        $finalStage = Join-Path $finalRoot 'runtime-stage'
        $finalStageSnapshot = Get-RuntimeStageSnapshot $finalStage $context $artifactAfter
        [void](Assert-BuildManifest $finalManifest $sourceBefore $artifactAfter $finalStageSnapshot $selectorProof)
        if ((Get-Sha256 $artifactSource) -ne [string]$artifactAfter.sha256) { throw 'The selected artifact changed after publication.' }
        Assert-SourceStateEqual $sourceBefore (Get-SourceState)
        $published = $true
        $script:Published = $true
        $summary = [ordered]@{
            schemaVersion = $script:SchemaVersion
            record = 'output-startup-build-producer'
            payloadFree = $true
            status = 'committed'
            backend = $Backend
            platform = $Platform.ToLowerInvariant()
            configuration = $Configuration
            outputBackend = $Backend
            utf16Backend = 'cpp'
            productionFlags = $false
            sourceFingerprintVerified = $true
            artifactBeforeVerified = $true
            artifactAfterVerified = $true
            runtimeStageVerified = $true
            selectorProofVerified = $true
            archiveExportsVerified = $true
            manifestVerified = $true
            transactionVerified = $true
            manifestSha256 = Get-Sha256 $finalManifest
            artifactSha256 = [string]$artifactAfter.sha256
            providerObjectSha256 = [string]$selectorProof.providerObjectSha256After
            rustArchiveSha256 = [string]$selectorProof.rustArchiveSha256
            selectorProofSha256 = [string]$selectorProof.selectorContractSha256
            runtimeStageReceiptSha256 = [string]$finalStageSnapshot.receiptSha256
            dependencyClosureSha256 = [string]$finalStageSnapshot.dependencyClosureSha256
        }
        $successSummary = $summary
    }
    catch {
        $failureType = switch ($script:Stage) {
            'package-plan' { 'package-plan' }
            'build' { 'build' }
            'selector-proof' { 'selector-proof' }
            'runtime-stage' { 'runtime-stage' }
            'manifest' { 'manifest' }
            'publication' { 'publication' }
            default { 'preflight' }
        }
        $failureStage = $script:Stage
        $script:ProducerExitCode = 1
    }
    finally {
        if (-not $published) {
            if ($movedFinalRoot) {
                try { [void](Remove-OwnedDirectory $finalRoot) }
                catch { $cleanupVerified = $false; $cleanupFailureCount++ }
            }
            if ($null -ne $transactionRoot) {
                try { [void](Remove-OwnedDirectory $transactionRoot) }
                catch { $cleanupVerified = $false; $cleanupFailureCount++ }
            }
        }
        if ($script:LockOwned) {
            try { [void](Release-ExclusiveLock) }
            catch { $cleanupVerified = $false; $cleanupFailureCount++ }
        }
    }
    $finalRootExists = if ($null -ne $finalRoot) { [bool](Test-Path -LiteralPath $finalRoot) } else { $false }
    $transactionRootExists = if ($null -ne $transactionRoot) { [bool](Test-Path -LiteralPath $transactionRoot) } else { $false }
    $lockExists = if ($null -ne $script:LockPath) { [bool](Test-Path -LiteralPath $script:LockPath) } else { $false }
    $unexpectedFinalRoot = (-not $published) -and $finalRootExists
    $remainingCount = 0
    if ($unexpectedFinalRoot) { $remainingCount++ }
    if ($transactionRootExists) { $remainingCount++ }
    if ($lockExists) { $remainingCount++ }
    if ($remainingCount -ne 0) { $cleanupVerified = $false }
    $cleanup = [ordered]@{
        attempted = $true
        verified = [bool]$cleanupVerified
        finalRootExists = $finalRootExists
        finalRootExpected = [bool]$published
        transactionRootExists = $transactionRootExists
        lockExists = $lockExists
        remainingCount = [int]$remainingCount
        failureCount = [int]$cleanupFailureCount
    }
    if ($null -ne $failureType -or -not $cleanupVerified) {
        $envelopeType = if ($cleanupVerified) { $failureType } else { 'cleanup-unverified' }
        try {
            Write-Output ((New-FailureEnvelope $envelopeType $failureStage $failureType $cleanup) | ConvertTo-Json -Compress -Depth 10)
        }
        catch { }
        $script:ProducerExitCode = 1
        return
    }
    Write-Output ($successSummary | ConvertTo-Json -Compress -Depth 10)
    $script:ProducerExitCode = 0
}

if ($SelfTest) {
    try {
        Invoke-SelfTest
        exit 0
    }
    catch {
        try { Write-Output ((New-FailureEnvelope 'self-test') | ConvertTo-Json -Compress -Depth 10) } catch { }
        exit 1
    }
}

Invoke-Producer
exit [int]$script:ProducerExitCode
