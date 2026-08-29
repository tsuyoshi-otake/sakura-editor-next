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
    [ValidateRange(1, 7200)]
    [int]$PackageTimeoutSeconds = 1800,
    [switch]$QualifiedFinalImage,
    [AllowNull()]
    [string]$FinalImageStageRoot,
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
$script:FailureCode = $null
$script:FailureSubstage = $null
$script:FinalImageStageRoot = $null
$script:FinalImageStageRootOwned = $false
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

public sealed class SakuraOutputStartupFileIdentityResult
{
    public bool Succeeded;
    public int ErrorCode;
    public ulong VolumeSerialNumber;
    public ulong FileIndex;
}

public sealed class SakuraOutputStartupFileDeleteResult
{
    public bool Succeeded;
    public bool IdentityAvailable;
    public bool IdentityMatched;
    public int ErrorCode;
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
    private const uint DELETE_ACCESS = 0x00010000;
    private const uint FILE_READ_ATTRIBUTES = 0x00000080;
    private const uint FILE_SHARE_READ = 0x00000001;
    private const uint OPEN_EXISTING = 3;
    private const uint FILE_FLAG_OPEN_REPARSE_POINT = 0x00200000;
    private const int FileDispositionInfo = 4;
    private static readonly IntPtr INVALID_HANDLE_VALUE = new IntPtr(-1);

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

    [StructLayout(LayoutKind.Sequential)]
    private struct BY_HANDLE_FILE_INFORMATION
    {
        public uint FileAttributes;
        public uint CreationTimeLow;
        public uint CreationTimeHigh;
        public uint LastAccessTimeLow;
        public uint LastAccessTimeHigh;
        public uint LastWriteTimeLow;
        public uint LastWriteTimeHigh;
        public uint VolumeSerialNumber;
        public uint FileSizeHigh;
        public uint FileSizeLow;
        public uint NumberOfLinks;
        public uint FileIndexHigh;
        public uint FileIndexLow;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct FILE_DISPOSITION_INFO
    {
        public byte DeleteFile;
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

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true, EntryPoint = "CreateFileW")]
    private static extern IntPtr CreateFile(
        string fileName,
        uint desiredAccess,
        uint shareMode,
        IntPtr securityAttributes,
        uint creationDisposition,
        uint flagsAndAttributes,
        IntPtr templateFile);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GetFileInformationByHandle(
        IntPtr file,
        out BY_HANDLE_FILE_INFORMATION fileInformation);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool SetFileInformationByHandle(
        IntPtr file,
        int fileInformationClass,
        IntPtr fileInformation,
        uint bufferSize);

    private static bool IsInvalidHandle(IntPtr handle)
    {
        return handle == IntPtr.Zero || handle == INVALID_HANDLE_VALUE;
    }

    private static SakuraOutputStartupFileIdentityResult ReadFileIdentity(IntPtr handle)
    {
        var result = new SakuraOutputStartupFileIdentityResult { Succeeded = false, ErrorCode = 0 };
        if (IsInvalidHandle(handle))
        {
            result.ErrorCode = 6;
            return result;
        }
        BY_HANDLE_FILE_INFORMATION information;
        if (!GetFileInformationByHandle(handle, out information))
        {
            result.ErrorCode = Marshal.GetLastWin32Error();
            return result;
        }
        result.Succeeded = true;
        result.VolumeSerialNumber = information.VolumeSerialNumber;
        result.FileIndex = ((ulong)information.FileIndexHigh << 32) | information.FileIndexLow;
        return result;
    }

    public static SakuraOutputStartupFileIdentityResult GetFileIdentity(IntPtr handle)
    {
        return ReadFileIdentity(handle);
    }

    public static SakuraOutputStartupFileDeleteResult DeleteOwnedFileIfIdentity(
        string path,
        ulong expectedVolumeSerialNumber,
        ulong expectedFileIndex)
    {
        var result = new SakuraOutputStartupFileDeleteResult
        {
            Succeeded = false,
            IdentityAvailable = false,
            IdentityMatched = false,
            ErrorCode = 0
        };
        IntPtr handle = CreateFile(
            path,
            DELETE_ACCESS | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ,
            IntPtr.Zero,
            OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT,
            IntPtr.Zero);
        if (IsInvalidHandle(handle))
        {
            result.ErrorCode = Marshal.GetLastWin32Error();
            return result;
        }
        try
        {
            var identity = ReadFileIdentity(handle);
            if (!identity.Succeeded)
            {
                result.ErrorCode = identity.ErrorCode;
                return result;
            }
            result.IdentityAvailable = true;
            result.IdentityMatched = identity.VolumeSerialNumber == expectedVolumeSerialNumber &&
                identity.FileIndex == expectedFileIndex;
            if (!result.IdentityMatched) return result;

            var disposition = new FILE_DISPOSITION_INFO { DeleteFile = 1 };
            IntPtr nativeDisposition = Marshal.AllocHGlobal(Marshal.SizeOf(typeof(FILE_DISPOSITION_INFO)));
            try
            {
                Marshal.StructureToPtr(disposition, nativeDisposition, false);
                if (!SetFileInformationByHandle(
                    handle,
                    FileDispositionInfo,
                    nativeDisposition,
                    unchecked((uint)Marshal.SizeOf(typeof(FILE_DISPOSITION_INFO))))
                )
                {
                    result.ErrorCode = Marshal.GetLastWin32Error();
                    return result;
                }
            }
            finally { Marshal.FreeHGlobal(nativeDisposition); }
            result.Succeeded = true;
            return result;
        }
        finally
        {
            if (!CloseHandle(handle) && result.Succeeded)
            {
                result.Succeeded = false;
                result.ErrorCode = Marshal.GetLastWin32Error();
            }
        }
    }

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

function Set-ProducerFailureContext {
    param(
        [Parameter(Mandatory = $true)] [string]$Code,
        [Parameter(Mandatory = $true)] [string]$Substage
    )
    $script:FailureCode = $Code
    $script:FailureSubstage = $Substage
}

function Invoke-NativeOutputCapture {
    param(
        [Parameter(Mandatory = $true)] [string]$Executable,
        [Parameter(Mandatory = $true)] [string[]]$Arguments,
        [ValidateRange(1, 64)] [int]$MaxLines = 8,
        [ValidateRange(1, 8192)] [int]$MaxChars = 2048
    )
    $oldErrorAction = $ErrorActionPreference
    $rawOutput = $null
    $exitCode = -1
    try {
        $ErrorActionPreference = 'SilentlyContinue'
        # Keep this assignment and LASTEXITCODE read adjacent.  Windows
        # PowerShell changes LASTEXITCODE when a pipeline runs after a native
        # process, even if that process itself returned zero.
        $rawOutput = & $Executable @Arguments 2>$null
        $exitCode = $LASTEXITCODE
    }
    catch {
        $exitCode = -1
    }
    finally { $ErrorActionPreference = $oldErrorAction }

    $lines = @($rawOutput)
    $charCount = [Int64]0
    $bounded = $true
    foreach ($entry in $lines) {
        if ($null -eq $entry) {
            $bounded = $false
            continue
        }
        $lineText = [string]$entry
        $charCount += [Int64]$lineText.Length
        if ($lineText.Length -gt $MaxChars -or $charCount -gt $MaxChars) { $bounded = $false }
    }
    if ($lines.Count -gt $MaxLines) { $bounded = $false }

    $retainedLines = @()
    $retainedCount = [Math]::Min($lines.Count, $MaxLines)
    for ($index = 0; $index -lt $retainedCount; $index++) { $retainedLines += [string]$lines[$index] }
    return [pscustomobject][ordered]@{
        exitCode = [int]$exitCode
        lines = $retainedLines
        lineCount = [int]$lines.Count
        charCount = [Int64]$charCount
        bounded = [bool]$bounded
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

function Resolve-RustcExecutable {
    Set-ProducerFailureContext 'RUST_TOOLCHAIN_RESOLUTION' 'rust-toolchain-resolve'
    try { return Resolve-Executable 'rustc.exe' } catch { }
    $rustup = Resolve-Executable 'rustup.exe'
    Push-Location $script:RepoRoot
    try {
        $captured = Invoke-NativeOutputCapture $rustup @('which', 'rustc') 4 1024
    }
    finally { Pop-Location }
    $rustcLine = if ($captured.lineCount -eq 1) { ([string]$captured.lines[0]).Trim() } else { $null }
    if ($captured.exitCode -ne 0 -or -not $captured.bounded -or
        $captured.lineCount -ne 1 -or [string]::IsNullOrWhiteSpace($rustcLine) -or
        $rustcLine.Length -gt 1024 -or $rustcLine -match '[\r\n]') {
        throw 'The selected Rust compiler is unavailable.'
    }
    $rustc = Get-CanonicalPath $rustcLine
    Assert-RegularFile $rustc
    return $rustc
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

function Convert-RustToolchainCaptureToIdentity {
    param([Parameter(Mandatory = $true)] [object]$Capture)
    if ($null -eq $Capture -or $null -eq $Capture.exitCode -or
        $null -eq $Capture.lineCount -or $null -eq $Capture.lines -or
        $null -eq $Capture.bounded) {
        Set-ProducerFailureContext 'RUST_TOOLCHAIN_IDENTITY_MALFORMED' 'rust-toolchain-identity'
        throw 'Rust toolchain identity capture is malformed.'
    }
    $lines = @($Capture.lines)
    if ([int]$Capture.exitCode -ne 0) {
        Set-ProducerFailureContext 'RUST_TOOLCHAIN_IDENTITY_COMMAND_FAILED' 'rust-toolchain-identity'
        throw 'Rust toolchain identity command failed.'
    }
    if (-not [bool]$Capture.bounded -or [int]$Capture.lineCount -ne 1 -or
        $lines.Count -ne 1 -or [string]::IsNullOrWhiteSpace([string]$lines[0])) {
        Set-ProducerFailureContext 'RUST_TOOLCHAIN_IDENTITY_MALFORMED' 'rust-toolchain-identity'
        throw 'Rust toolchain identity is malformed.'
    }
    $identity = ([string]$lines[0]).Trim()
    if ($identity.Length -gt 512 -or $identity -match '[\r\n]') {
        Set-ProducerFailureContext 'RUST_TOOLCHAIN_IDENTITY_MALFORMED' 'rust-toolchain-identity'
        throw 'Rust toolchain identity is malformed.'
    }
    return $identity
}

function Get-RustToolchainIdentity {
    Set-ProducerFailureContext 'RUST_TOOLCHAIN_IDENTITY' 'rust-toolchain-identity'
    $rustc = Resolve-RustcExecutable
    $captured = Invoke-NativeOutputCapture $rustc @('--version') 4 512
    return Convert-RustToolchainCaptureToIdentity $captured
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

function New-QualifiedObserverCommand {
    param(
        [Parameter(Mandatory = $true)] [string]$PythonLauncher,
        [Parameter(Mandatory = $true)] [string]$BuildScript,
        [Parameter(Mandatory = $true)] [string]$Context,
        [Parameter(Mandatory = $true)] [int]$Jobs,
        [Parameter(Mandatory = $true)] [int]$Timeout,
        [Parameter(Mandatory = $true)] [int]$PackageTimeout,
        [Parameter(Mandatory = $true)] [ValidateSet('cpp', 'rust')] [string]$ProviderBackend,
        [Parameter(Mandatory = $true)] [string]$FinalImageRoot,
        [Parameter(Mandatory = $true)] [string]$NativeEvidenceOutput
    )
    return ('{0} -3 {1} --format json inventory observe-product --context {2} --product sakura_app --jobs {3} --timeout-seconds {4} --package-timeout-seconds {5} --rebuild --final-image-backend {6} --final-image-stage-root {7} --output {8}' -f
        (ConvertTo-WindowsCommandLineArgument $PythonLauncher),
        (ConvertTo-WindowsCommandLineArgument $BuildScript),
        (ConvertTo-WindowsCommandLineArgument $Context),
        $Jobs,
        $Timeout,
        $PackageTimeout,
        (ConvertTo-WindowsCommandLineArgument $ProviderBackend),
        (ConvertTo-WindowsCommandLineArgument $FinalImageRoot),
        (ConvertTo-WindowsCommandLineArgument $NativeEvidenceOutput))
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
    if ($PackageTimeoutSeconds -lt 1 -or $PackageTimeoutSeconds -gt 7200) { throw 'PackageTimeoutSeconds is outside its bounded range.' }
    if ($QualifiedFinalImage -and [string]::IsNullOrWhiteSpace($FinalImageStageRoot)) {
        throw 'Qualified final-image production requires FinalImageStageRoot.'
    }
    if (-not $QualifiedFinalImage -and -not [string]::IsNullOrWhiteSpace($FinalImageStageRoot)) {
        throw 'FinalImageStageRoot requires QualifiedFinalImage.'
    }
}

function Assert-QualifiedSourceState {
    param([Parameter(Mandatory = $true)] [object]$Source)
    $statusLineCount = if ($null -ne $Source) { Get-PropertyValue $Source @('statusLineCount') } else { $null }
    if ($null -eq $Source -or
        (Get-PropertyValue $Source @('head')) -notmatch '^[0-9a-fA-F]{40}$' -or
        (Get-PropertyValue $Source @('dirty')) -isnot [bool] -or
        [bool](Get-PropertyValue $Source @('dirty')) -or
        $null -eq $statusLineCount -or
        $statusLineCount -is [bool] -or
        [string]$statusLineCount -notmatch '^[0-9]+$' -or
        [int]$statusLineCount -ne 0 -or
        -not (Test-Sha256 (Get-PropertyValue $Source @('statusSha256')))) {
        throw 'Qualified final-image production requires a clean exact source state.'
    }
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

function Get-OwnedFileIdentity {
    param([Parameter(Mandatory = $true)] [IO.FileStream]$Stream)
    if ($null -eq $Stream -or $Stream.SafeFileHandle.IsClosed -or $Stream.SafeFileHandle.IsInvalid) {
        throw 'The producer file identity handle is closed or invalid.'
    }
    $nativeResult = [SakuraOutputStartupNative]::GetFileIdentity($Stream.SafeFileHandle.DangerousGetHandle())
    if ($null -eq $nativeResult) { throw 'The producer file identity result was empty.' }
    $nativeKeys = @($nativeResult.PSObject.Properties | ForEach-Object { $_.Name })
    $expectedNativeKeys = @('ErrorCode', 'FileIndex', 'Succeeded', 'VolumeSerialNumber')
    [Array]::Sort($nativeKeys, [StringComparer]::Ordinal)
    [Array]::Sort($expectedNativeKeys, [StringComparer]::Ordinal)
    if (($nativeKeys -join '|') -cne ($expectedNativeKeys -join '|') -or
        $nativeResult.Succeeded -isnot [bool] -or
        $nativeResult.ErrorCode -isnot [int] -or
        $nativeResult.VolumeSerialNumber -isnot [UInt64] -or
        $nativeResult.FileIndex -isnot [UInt64]) {
        throw 'The producer file identity result schema is not exact.'
    }
    if (-not [bool]$nativeResult.Succeeded) {
        throw ('GetFileInformationByHandle failed with Win32 error {0}.' -f [int]$nativeResult.ErrorCode)
    }
    return [pscustomobject][ordered]@{
        volumeSerialNumber = [UInt64]$nativeResult.VolumeSerialNumber
        fileIndex = [UInt64]$nativeResult.FileIndex
    }
}

function Test-OwnedFileIdentityEqual {
    param(
        [Parameter(Mandatory = $true)] [AllowNull()] [object]$Expected,
        [Parameter(Mandatory = $true)] [AllowNull()] [object]$Actual
    )
    if ($null -eq $Expected -or $null -eq $Actual) { return $false }
    try {
        $expectedVolume = Get-PropertyValue $Expected @('volumeSerialNumber')
        $expectedIndex = Get-PropertyValue $Expected @('fileIndex')
        $actualVolume = Get-PropertyValue $Actual @('volumeSerialNumber')
        $actualIndex = Get-PropertyValue $Actual @('fileIndex')
        if ($null -eq $expectedVolume -or $null -eq $expectedIndex -or
            $null -eq $actualVolume -or $null -eq $actualIndex) { return $false }
        return [UInt64]$expectedVolume -eq [UInt64]$actualVolume -and
            [UInt64]$expectedIndex -eq [UInt64]$actualIndex
    }
    catch { return $false }
}

function Assert-OwnedFileIdentity {
    param(
        [Parameter(Mandatory = $true)] [IO.FileStream]$Stream,
        [Parameter(Mandatory = $true)] [AllowNull()] [object]$ExpectedIdentity,
        [Parameter(Mandatory = $true)] [ValidatePattern('^[a-z][a-z0-9-]{0,63}$')] [string]$BarrierSubstage
    )
    try { $actualIdentity = Get-OwnedFileIdentity $Stream }
    catch {
        Set-ProducerFailureContext 'OUTPUT_FINAL_IMAGE_VERIFY_FILE_IDENTITY_UNAVAILABLE' $BarrierSubstage
        throw
    }
    if (-not (Test-OwnedFileIdentityEqual $ExpectedIdentity $actualIdentity)) {
        Set-ProducerFailureContext 'OUTPUT_FINAL_IMAGE_VERIFY_FILE_IDENTITY_CHANGED' $BarrierSubstage
        throw 'The final-image verification output file identity changed before it was read.'
    }
    return $actualIdentity
}

function Open-VerifiedOwnedOutput {
    param(
        [Parameter(Mandatory = $true)] [string]$Path,
        [Parameter(Mandatory = $true)] [AllowNull()] [object]$ExpectedIdentity,
        [Parameter(Mandatory = $true)] [ValidatePattern('^[a-z][a-z0-9-]{0,63}$')] [string]$BarrierSubstage
    )
    Assert-RegularFile $Path
    Assert-NoReparseAncestors $Path
    $stream = $null
    try {
        # Sharing read only keeps the identity-checked file from being
        # replaced, rewritten, or deleted while the bounded parser reads it.
        $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
        [void](Assert-OwnedFileIdentity $stream $ExpectedIdentity $BarrierSubstage)
        return $stream
    }
    catch {
        if ($null -ne $stream) {
            try { $stream.Dispose() } catch { }
            $stream = $null
        }
        throw
    }
}

function Remove-VerifiedOwnedOutput {
    param(
        [Parameter(Mandatory = $true)] [string]$Path,
        [Parameter(Mandatory = $true)] [AllowNull()] [object]$ExpectedIdentity,
        [Parameter(Mandatory = $true)] [ValidatePattern('^[a-z][a-z0-9-]{0,63}$')] [string]$BarrierSubstage
    )
    $previousFailureCode = $script:FailureCode
    $previousFailureSubstage = $script:FailureSubstage
    Set-ProducerFailureContext 'OUTPUT_FINAL_IMAGE_VERIFY_FILE_IDENTITY_UNAVAILABLE' $BarrierSubstage
    try {
        Assert-RegularFile $Path
        Assert-NoReparseAncestors $Path
    }
    catch {
        Set-ProducerFailureContext 'OUTPUT_FINAL_IMAGE_VERIFY_FILE_IDENTITY_UNAVAILABLE' $BarrierSubstage
        throw
    }
    $expectedVolumeSerialNumber = [UInt64](Get-PropertyValue $ExpectedIdentity @('volumeSerialNumber'))
    $expectedFileIndex = [UInt64](Get-PropertyValue $ExpectedIdentity @('fileIndex'))
    $deleteResult = [SakuraOutputStartupNative]::DeleteOwnedFileIfIdentity($Path, $expectedVolumeSerialNumber, $expectedFileIndex)
    if ($null -eq $deleteResult) { throw 'The producer owned-file delete result was empty.' }
    $deleteKeys = @($deleteResult.PSObject.Properties | ForEach-Object { $_.Name })
    $expectedDeleteKeys = @('ErrorCode', 'IdentityAvailable', 'IdentityMatched', 'Succeeded')
    [Array]::Sort($deleteKeys, [StringComparer]::Ordinal)
    [Array]::Sort($expectedDeleteKeys, [StringComparer]::Ordinal)
    if (($deleteKeys -join '|') -cne ($expectedDeleteKeys -join '|') -or
        $deleteResult.Succeeded -isnot [bool] -or
        $deleteResult.IdentityAvailable -isnot [bool] -or
        $deleteResult.IdentityMatched -isnot [bool] -or
        $deleteResult.ErrorCode -isnot [int]) {
        throw 'The producer owned-file delete result schema is not exact.'
    }
    if (-not [bool]$deleteResult.IdentityAvailable) {
        Set-ProducerFailureContext 'OUTPUT_FINAL_IMAGE_VERIFY_FILE_IDENTITY_UNAVAILABLE' $BarrierSubstage
        throw ('Owned final-image verification output identity could not be reopened (Win32 error {0}).' -f [int]$deleteResult.ErrorCode)
    }
    if (-not [bool]$deleteResult.IdentityMatched) {
        Set-ProducerFailureContext 'OUTPUT_FINAL_IMAGE_VERIFY_FILE_IDENTITY_CHANGED' $BarrierSubstage
        throw 'The final-image verification output file identity changed before cleanup.'
    }
    if (-not [bool]$deleteResult.Succeeded) {
        Set-ProducerFailureContext 'OUTPUT_FINAL_IMAGE_VERIFY_FILE_CLEANUP_FAILED' $BarrierSubstage
        throw ('Owned final-image verification output cleanup failed (Win32 error {0}).' -f [int]$deleteResult.ErrorCode)
    }
    if (Test-Path -LiteralPath $Path) {
        Set-ProducerFailureContext 'OUTPUT_FINAL_IMAGE_VERIFY_FILE_CLEANUP_FAILED' $BarrierSubstage
        throw 'Owned final-image verification output cleanup did not complete.'
    }
    $script:FailureCode = $previousFailureCode
    $script:FailureSubstage = $previousFailureSubstage
    return $true
}

function Read-OwnedBoundedUtf8Text {
    param(
        [Parameter(Mandatory = $true)] [IO.Stream]$Stream,
        [Parameter(Mandatory = $true)] [ValidateRange(1, 1048576)] [Int64]$MaximumBytes
    )
    if (-not $Stream.CanRead -or -not $Stream.CanSeek) { throw 'The owned final-image verification output stream is not readable.' }
    [void]$Stream.Flush()
    [void]$Stream.Seek(0, [IO.SeekOrigin]::Begin)
    $buffer = New-Object byte[] 8192
    $captured = [IO.MemoryStream]::new()
    try {
        while ($true) {
            [Int64]$remaining = $MaximumBytes - $captured.Length
            if ($remaining -lt 0) { throw 'Bounded final-image verification output exceeded its maximum size.' }
            [Int32]$readLimit = [int][Math]::Min([Int64]$buffer.Length, $remaining + 1)
            $read = $Stream.Read($buffer, 0, $readLimit)
            if ($read -le 0) { break }
            if ($captured.Length + $read -gt $MaximumBytes) {
                throw 'Bounded final-image verification output exceeded its maximum size.'
            }
            $captured.Write($buffer, 0, $read)
        }
        return (New-Object Text.UTF8Encoding($false, $true)).GetString($captured.ToArray())
    }
    finally {
        $captured.Dispose()
    }
}

function Invoke-QualifiedFinalImageVerification {
    param(
        [Parameter(Mandatory = $true)] [string]$PythonLauncher,
        [Parameter(Mandatory = $true)] [string]$BuildScript,
        [Parameter(Mandatory = $true)] [string]$NativeEvidencePath,
        [Parameter(Mandatory = $true)] [string]$OwnedStageRoot,
        [Parameter(Mandatory = $true)] [ValidateSet('cpp', 'rust')] [string]$ExpectedBackend,
        [Parameter(Mandatory = $true)] [ValidateSet('x64')] [string]$ExpectedPlatform,
        [Parameter(Mandatory = $true)] [ValidateSet('Debug', 'Release')] [string]$ExpectedConfiguration,
        [Parameter(Mandatory = $true)] [object]$ExpectedArtifact,
        [Parameter(Mandatory = $true)] [string]$ComSpec,
        [Parameter(Mandatory = $true)] [string]$EnvironmentBlock,
        [Parameter(Mandatory = $true)] [int]$TimeoutSeconds,
        [Parameter(Mandatory = $true)] [ValidatePattern('^[a-z][a-z0-9-]{0,63}$')] [string]$BarrierSubstage
    )
    Assert-RegularDirectory $OwnedStageRoot
    Assert-NoReparseAncestors $OwnedStageRoot
    $nativeEvidenceCanonical = Get-CanonicalPath $NativeEvidencePath
    Assert-RegularFile $nativeEvidenceCanonical
    $nativeEvidenceParent = Split-Path -Parent $nativeEvidenceCanonical
    Assert-RegularDirectory $nativeEvidenceParent
    Assert-NoReparseAncestors $nativeEvidenceParent
    $temporary = Join-Path $nativeEvidenceParent ('.output-final-image-verify-{0}.json' -f ([Guid]::NewGuid().ToString('N')))
    Assert-NoReparseAncestors $temporary
    $temporaryStream = $null
    $temporaryOwned = $false
    $temporaryIdentity = $null
    try {
        $temporaryStream = [IO.FileStream]::new(
            $temporary,
            [IO.FileMode]::CreateNew,
            [IO.FileAccess]::ReadWrite,
            [IO.FileShare]::ReadWrite,
            4096,
            [IO.FileOptions]::SequentialScan)
        $temporaryOwned = $true
        try { $temporaryIdentity = Get-OwnedFileIdentity $temporaryStream }
        catch {
            Set-ProducerFailureContext 'OUTPUT_FINAL_IMAGE_VERIFY_FILE_IDENTITY_UNAVAILABLE' $BarrierSubstage
            throw
        }
        # cmd.exe must be able to open the path for redirection.  Close the
        # CreateNew handle before invoking it.  The saved handle identity is
        # checked again before any child output is parsed.
        $temporaryStream.Dispose()
        $temporaryStream = $null
        $commandText = '{0} -3 {1} --format json evidence output-final-image-verify --native-evidence {2} --stage-root {3} --backend {4} --platform {5} --configuration {6} --artifact-sha256 {7} --artifact-size-bytes {8}' -f
            (ConvertTo-WindowsCommandLineArgument $PythonLauncher),
            (ConvertTo-WindowsCommandLineArgument $BuildScript),
            (ConvertTo-WindowsCommandLineArgument $nativeEvidenceCanonical),
            (ConvertTo-WindowsCommandLineArgument $OwnedStageRoot),
            (ConvertTo-WindowsCommandLineArgument $ExpectedBackend),
            (ConvertTo-WindowsCommandLineArgument $ExpectedPlatform),
            (ConvertTo-WindowsCommandLineArgument $ExpectedConfiguration),
            (ConvertTo-WindowsCommandLineArgument ([string]$ExpectedArtifact.sha256)),
            ([UInt64]$ExpectedArtifact.sizeBytes).ToString([Globalization.CultureInfo]::InvariantCulture)
        $commandText = $commandText + ' > ' + (ConvertTo-WindowsCommandLineArgument $temporary) + ' 2> NUL'
        $commandError = $null
        Set-ProducerFailureContext 'OUTPUT_FINAL_IMAGE_VERIFY_OUTPUT_INVALID' $BarrierSubstage
        try {
            [void](Invoke-OwnedCommand $ComSpec (New-CmdCommandLine $commandText $ComSpec) $script:RepoRoot $EnvironmentBlock $TimeoutSeconds)
        }
        catch {
            $commandError = $_
        }
        if ($null -ne $commandError) {
            try {
                $temporaryStream = Open-VerifiedOwnedOutput $temporary $temporaryIdentity $BarrierSubstage
                $failureText = Read-OwnedBoundedUtf8Text $temporaryStream (16 * 1024)
                $failureResult = $failureText | ConvertFrom-Json -ErrorAction Stop
                if ($null -eq $failureResult -or $failureResult -is [Array]) { throw 'Failure output is not an object.' }
                $failureKeys = @($failureResult.PSObject.Properties | ForEach-Object { $_.Name })
                $expectedFailureKeys = @('failureCode', 'ok', 'payloadFree', 'record')
                [Array]::Sort($failureKeys, [StringComparer]::Ordinal)
                [Array]::Sort($expectedFailureKeys, [StringComparer]::Ordinal)
                if (($failureKeys -join '|') -cne ($expectedFailureKeys -join '|') -or
                    $failureResult.ok -isnot [bool] -or [bool]$failureResult.ok -or
                    $failureResult.payloadFree -isnot [bool] -or -not [bool]$failureResult.payloadFree -or
                    [string]$failureResult.record -cne 'output-final-image-binding-validation' -or
                    [string]$failureResult.failureCode -cnotmatch '^[A-Z][A-Z0-9_]{0,127}$') {
                    throw 'Failure output schema is not exact.'
                }
                Set-ProducerFailureContext ([string]$failureResult.failureCode) $BarrierSubstage
            }
            catch { }
            throw $commandError
        }
        $temporaryStream = Open-VerifiedOwnedOutput $temporary $temporaryIdentity $BarrierSubstage
        $jsonText = Read-OwnedBoundedUtf8Text $temporaryStream (64 * 1024)
        try {
            $result = $jsonText | ConvertFrom-Json -ErrorAction Stop
        }
        catch { throw 'Canonical final-image verification output is not valid JSON.' }
        $expectedKeys = @(
            'backend',
            'boundNativeEvidenceSha256',
            'configuration',
            'files',
            'ok',
            'payloadFree',
            'platform',
            'provider',
            'receiptPath',
            'receiptSha256',
            'record',
            'sourceNativeEvidenceSha256',
            'stageId'
        )
        if ($null -eq $result -or $result -is [Array]) { throw 'Canonical final-image verification output is not a success object.' }
        $actualKeys = @($result.PSObject.Properties | ForEach-Object { $_.Name })
        [Array]::Sort($expectedKeys, [StringComparer]::Ordinal)
        [Array]::Sort($actualKeys, [StringComparer]::Ordinal)
        if (($actualKeys -join '|') -cne ($expectedKeys -join '|')) { throw 'Canonical final-image verification output schema is not exact.' }
        if ($result.ok -isnot [bool] -or -not [bool]$result.ok -or
            $result.payloadFree -isnot [bool] -or -not [bool]$result.payloadFree -or
            [string]$result.record -cne 'output-final-image-binding-validation' -or
            [string]$result.backend -cne $ExpectedBackend -or
            [string]$result.platform -cne $ExpectedPlatform -or
            [string]$result.configuration -cne $ExpectedConfiguration) {
            throw 'Canonical final-image verification output selectors are not exact.'
        }
        foreach ($hashName in @('boundNativeEvidenceSha256', 'sourceNativeEvidenceSha256', 'receiptSha256')) {
            $hashValue = [string](Get-PropertyValue $result @($hashName))
            if ($hashValue -notmatch '^(?i:sha256:)[0-9a-fA-F]{64}$') {
                throw 'Canonical final-image verification output contains an invalid hash.'
            }
        }
        foreach ($identityName in @('stageId', 'receiptPath')) {
            $identityValue = [string](Get-PropertyValue $result @($identityName))
            if ([string]::IsNullOrWhiteSpace($identityValue) -or $identityValue -ne $identityValue.Trim() -or
                $identityValue.Length -gt 2048 -or $identityValue -match '[\x00-\x1f]') {
                throw 'Canonical final-image verification output contains an invalid identity.'
            }
        }
        $files = Get-PropertyValue $result @('files')
        if ($null -eq $files) { throw 'Canonical final-image verification output has no file identities.' }
        $fileKeys = @($files.PSObject.Properties | ForEach-Object { $_.Name })
        [Array]::Sort($fileKeys, [StringComparer]::Ordinal)
        if (($fileKeys -join '|') -cne 'exe|map') { throw 'Canonical final-image verification output file schema is not exact.' }
        foreach ($fileName in @('exe', 'map')) {
            $file = Get-PropertyValue $files @($fileName)
            if ($null -eq $file) { throw 'Canonical final-image verification output file identity is missing.' }
            $keys = @($file.PSObject.Properties | ForEach-Object { $_.Name })
            [Array]::Sort($keys, [StringComparer]::Ordinal)
            if (($keys -join '|') -cne 'path|sha256|sizeBytes') { throw 'Canonical final-image verification output file schema is not exact.' }
            $filePath = [string](Get-PropertyValue $file @('path'))
            if ([string]::IsNullOrWhiteSpace($filePath) -or $filePath -ne $filePath.Trim() -or
                $filePath.Length -gt 2048 -or $filePath -match '[\x00-\x1f]') {
                throw 'Canonical final-image verification output file path is invalid.'
            }
            $fileHash = [string](Get-PropertyValue $file @('sha256'))
            if ($fileHash -notmatch '^(?i:sha256:)[0-9a-fA-F]{64}$') { throw 'Canonical final-image verification output file hash is invalid.' }
            $fileSizeValue = Get-PropertyValue $file @('sizeBytes')
            if ($null -eq $fileSizeValue -or $fileSizeValue -is [bool] -or [string]$fileSizeValue -notmatch '^[0-9]+$') {
                throw 'Canonical final-image verification output file size is invalid.'
            }
            try { [UInt64]$fileSize = $fileSizeValue } catch { throw 'Canonical final-image verification output file size is invalid.' }
            if (($fileName -eq 'exe' -and $fileHash.Substring(7).ToLowerInvariant() -cne ([string]$ExpectedArtifact.sha256).ToLowerInvariant()) -or
                ($fileName -eq 'exe' -and [UInt64]$fileSize -ne [UInt64]$ExpectedArtifact.sizeBytes)) {
                throw 'Canonical final-image verification output executable identity is stale.'
            }
        }
        $provider = Get-PropertyValue $result @('provider')
        if ($null -eq $provider) { throw 'Canonical final-image verification output has no provider identity.' }
        $providerKeys = @($provider.PSObject.Properties | ForEach-Object { $_.Name })
        [Array]::Sort($providerKeys, [StringComparer]::Ordinal)
        if (($providerKeys -join '|') -cne 'mapSha256|mapSizeBytes|memberCount|symbolCount') {
            throw 'Canonical final-image verification output provider schema is not exact.'
        }
        $providerMapHash = [string](Get-PropertyValue $provider @('mapSha256'))
        if ($providerMapHash -notmatch '^(?i:sha256:)[0-9a-fA-F]{64}$') { throw 'Canonical final-image verification output provider MAP hash is invalid.' }
        foreach ($countName in @('mapSizeBytes', 'memberCount', 'symbolCount')) {
            $countValue = Get-PropertyValue $provider @($countName)
            if ($null -eq $countValue -or $countValue -is [bool] -or [string]$countValue -notmatch '^[0-9]+$') {
                throw 'Canonical final-image verification output provider count is invalid.'
            }
            try { [UInt64]$unusedCount = $countValue } catch { throw 'Canonical final-image verification output provider count is invalid.' }
        }
        $mapFile = Get-PropertyValue $files @('map')
        if ([string](Get-PropertyValue $mapFile @('sha256')) -cne $providerMapHash -or
            [UInt64](Get-PropertyValue $mapFile @('sizeBytes')) -ne [UInt64](Get-PropertyValue $provider @('mapSizeBytes'))) {
            throw 'Canonical final-image verification output provider MAP identity is stale.'
        }
        return $result
    }
    finally {
        if ($null -ne $temporaryStream) {
            $temporaryStream.Dispose()
            $temporaryStream = $null
        }
        if ($temporaryOwned -and $null -ne $temporaryIdentity) {
            [void](Remove-VerifiedOwnedOutput $temporary $temporaryIdentity $BarrierSubstage)
        }
    }
}

function Assert-QualifiedFinalImageVerificationEqual {
    param(
        [Parameter(Mandatory = $true)] [object]$Expected,
        [Parameter(Mandatory = $true)] [object]$Actual
    )
    foreach ($name in @(
        'backend',
        'platform',
        'configuration',
        'boundNativeEvidenceSha256',
        'sourceNativeEvidenceSha256',
        'stageId',
        'receiptPath',
        'receiptSha256'
    )) {
        if ([string](Get-PropertyValue $Expected @($name)) -cne [string](Get-PropertyValue $Actual @($name))) {
            throw 'Canonical final-image verification identity changed during producer transaction.'
        }
    }
    foreach ($fileName in @('exe', 'map')) {
        $expectedFile = Get-PropertyValue (Get-PropertyValue $Expected @('files')) @($fileName)
        $actualFile = Get-PropertyValue (Get-PropertyValue $Actual @('files')) @($fileName)
        foreach ($name in @('path', 'sha256', 'sizeBytes')) {
            if ([string](Get-PropertyValue $expectedFile @($name)) -cne [string](Get-PropertyValue $actualFile @($name))) {
                throw 'Canonical final-image verification file identity changed during producer transaction.'
            }
        }
    }
    $expectedProvider = Get-PropertyValue $Expected @('provider')
    $actualProvider = Get-PropertyValue $Actual @('provider')
    foreach ($name in @('memberCount', 'symbolCount', 'mapSha256', 'mapSizeBytes')) {
        if ([string](Get-PropertyValue $expectedProvider @($name)) -cne [string](Get-PropertyValue $actualProvider @($name))) {
            throw 'Canonical final-image verification provider identity changed during producer transaction.'
        }
    }
    return $true
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

function Get-NormalizedProviderSymbols {
    param(
        [Parameter(Mandatory = $true)] [string]$Text,
        [switch]$Definitions
    )
    $symbols = New-Object Collections.Generic.List[string]
    foreach ($line in @($Text -split "`r?`n")) {
        $pattern = if ($Definitions) {
            '(?i)^\s*[0-9A-F]+\s+[0-9A-F]+\s+SECT[0-9A-F]+\s+notype\s+\(\)\s+External\s+\|\s+(sakura_output_provider_[A-Za-z0-9_]+)\s*$'
        }
        else {
            '(?i)\bUNDEF\b.*\|\s*(sakura_output_provider_[A-Za-z0-9_]+)\s*$'
        }
        $match = [regex]::Match([string]$line, $pattern)
        if ($match.Success) {
            $symbol = $match.Groups[1].Value.ToLowerInvariant()
            if (-not $symbols.Contains($symbol)) { [void]$symbols.Add($symbol) }
        }
    }
    $symbols.Sort([StringComparer]::Ordinal)
    return $symbols.ToArray()
}

function Get-ProviderCompileSelector {
    param(
        [Parameter(Mandatory = $true)] [string]$Text,
        [Parameter(Mandatory = $true)] [string]$SourceName
    )
    $records = @($Text -split '(?m)(?=^\^)')
    $commands = New-Object Collections.Generic.List[string]
    foreach ($record in $records) {
        $lines = @($record -split "`r?`n")
        if ($lines.Count -lt 2) { continue }
        $sourceLine = $lines[0].Trim().TrimStart([char]0xFEFF)
        if (-not $sourceLine.StartsWith('^', [StringComparison]::Ordinal)) { continue }
        $sourcePath = $sourceLine.Substring(1).Trim()
        if ([IO.Path]::GetFileName($sourcePath) -ine $SourceName) { continue }
        $command = (($lines | Select-Object -Skip 1) -join ' ').Trim()
        if ([string]::IsNullOrWhiteSpace($command)) { continue }
        [void]$commands.Add($command)
    }
    if ($commands.Count -ne 1) {
        throw 'Compiler command log does not contain exactly one provider source command.'
    }
    $command = $commands[0]
    $hasGl = [bool]($command -match '(?i)(?:^|\s)/GL(?=\s|$)')
    if (-not $hasGl) { throw 'Provider source compile command is not an LTCG /GL command.' }
    $rustSelectorCount = [regex]::Matches(
        $command,
        '(?i)(?:^|\s)/D\s+SAKURA_OUTPUT_BACKEND_RUST(?=\s|$)'
    ).Count
    return [pscustomobject][ordered]@{
        hasGl = $hasGl
        rustSelectorDefineCount = [int]$rustSelectorCount
    }
}

function Assert-ProviderCompileSelector {
    param(
        [Parameter(Mandatory = $true)] [string]$Text,
        [Parameter(Mandatory = $true)] [string]$SourceName,
        [Parameter(Mandatory = $true)] [ValidateSet('cpp', 'rust')] [string]$ExpectedBackend
    )
    $selector = Get-ProviderCompileSelector $Text $SourceName
    if (($ExpectedBackend -eq 'rust' -and $selector.rustSelectorDefineCount -ne 1) -or
        ($ExpectedBackend -eq 'cpp' -and $selector.rustSelectorDefineCount -ne 0)) {
        throw 'Provider compile command selector does not match the requested backend.'
    }
    return $selector
}

function Assert-ProviderObjectFormat {
    param(
        [Parameter(Mandatory = $true)] [string]$DumpbinText,
        [Parameter(Mandatory = $true)] [ValidateSet('Debug', 'Release')] [string]$ExpectedConfiguration
    )
    $objectAnonymous = [bool]($DumpbinText -match '(?im)^\s*File Type:\s+ANONYMOUS OBJECT\s*$')
    if ($ExpectedConfiguration -eq 'Release' -and -not $objectAnonymous) {
        throw 'Release provider object is expected to be an MSVC LTCG anonymous object.'
    }
    if ($ExpectedConfiguration -ne 'Release' -and $objectAnonymous) {
        throw 'Provider object proof unexpectedly became anonymous outside Release.'
    }
    return [ordered]@{
        anonymous = $objectAnonymous
        format = if ($objectAnonymous) { 'msvc-ltcg-anonymous' } else { 'coff-symbols' }
    }
}

function New-SelectorProof {
    param(
        [Parameter(Mandatory = $true)] [object]$ProviderObjectBefore,
        [Parameter(Mandatory = $true)] [object]$ProviderObjectAfter,
        [string[]]$UnresolvedProviderSymbols = @(),
        [ValidateSet('environment-selector-verified', 'dumpbin-unresolved-refs-verified', 'msvc-ltcg-compile-selector-verified')]
        [string]$ProofResult = 'environment-selector-verified',
        [string]$VerificationMethod = 'environment-selector',
        [string]$ProviderObjectFormat = 'coff-symbols',
        [AllowNull()] [object]$CompileLogBefore = $null,
        [AllowNull()] [object]$CompileLogAfter = $null,
        [AllowNull()] [object]$CompileSelector = $null
    )
    if ($null -eq $ProviderObjectAfter -or -not (Test-Sha256 $ProviderObjectAfter.sha256) -or
        [UInt64]$ProviderObjectAfter.sizeBytes -lt 1) {
        throw 'The selected build did not produce the Output provider object proof.'
    }
    if ($ProviderObjectBefore.exists -and -not (Test-Sha256 $ProviderObjectBefore.sha256)) {
        throw 'The pre-build Output provider object identity is invalid.'
    }
    $normalizedSymbols = @($UnresolvedProviderSymbols | ForEach-Object { ([string]$_).ToLowerInvariant() } | Sort-Object -Unique)
    $compileBeforeExists = $null -ne $CompileLogBefore -and [bool]$CompileLogBefore.exists
    $compileAfterExists = $null -ne $CompileLogAfter -and [bool]$CompileLogAfter.exists
    $compileBeforeHash = if ($compileBeforeExists) { ([string]$CompileLogBefore.sha256).ToLowerInvariant() } else { 'missing' }
    $compileAfterHash = if ($compileAfterExists) { ([string]$CompileLogAfter.sha256).ToLowerInvariant() } else { 'missing' }
    $compileBeforeSize = if ($compileBeforeExists) { [UInt64]$CompileLogBefore.sizeBytes } else { [UInt64]0 }
    $compileAfterSize = if ($compileAfterExists) { [UInt64]$CompileLogAfter.sizeBytes } else { [UInt64]0 }
    if (($compileBeforeExists -and (-not (Test-Sha256 $CompileLogBefore.sha256) -or $compileBeforeSize -lt 1)) -or
        ($compileAfterExists -and (-not (Test-Sha256 $CompileLogAfter.sha256) -or $compileAfterSize -lt 1))) {
        throw 'The Output provider compile log identity is invalid.'
    }
    $compileHasGl = if ($null -ne $CompileSelector) { [bool]$CompileSelector.hasGl } else { $false }
    $compileRustSelectorCount = if ($null -ne $CompileSelector) { [int]$CompileSelector.rustSelectorDefineCount } else { 0 }
    $canonical = if ($ProofResult -eq 'msvc-ltcg-compile-selector-verified') {
        'output={0}|utf16=cpp|output-production=false|utf16-production=false|telemetry=false|listings=false|result={1}|method={2}|symbols=|object-after={3}|object-format={4}|compile-log-before={5}|compile-log-before-size={6}|compile-log-after={7}|compile-log-after-size={8}|compile-gl={9}|compile-rust-selector-count={10}' -f
            $Backend, $ProofResult, $VerificationMethod, ([string]$ProviderObjectAfter.sha256).ToLowerInvariant(), $ProviderObjectFormat,
            $compileBeforeHash, $compileBeforeSize, $compileAfterHash, $compileAfterSize, $compileHasGl, $compileRustSelectorCount
    }
    else {
        'output={0}|utf16=cpp|output-production=false|utf16-production=false|telemetry=false|listings=false|result={1}|symbols={2}|object-after={3}' -f
            $Backend, $ProofResult, ($normalizedSymbols -join ','), ([string]$ProviderObjectAfter.sha256).ToLowerInvariant()
    }
    return [ordered]@{
        result = $ProofResult
        outputBackend = $Backend
        utf16Backend = 'cpp'
        outputProductionPackage = $false
        utf16ProductionPackage = $false
        utf16BenchmarkTelemetry = $false
        assemblyListings = $false
        verificationMethod = $VerificationMethod
        providerObjectFormat = $ProviderObjectFormat
        providerObjectSha256Before = if ($ProviderObjectBefore.exists) { [string]$ProviderObjectBefore.sha256 } else { $null }
        providerObjectSha256After = [string]$ProviderObjectAfter.sha256
        providerObjectSizeBytesAfter = [UInt64]$ProviderObjectAfter.sizeBytes
        unresolvedProviderSymbols = $normalizedSymbols
        unresolvedProviderSymbolCount = [int]$normalizedSymbols.Count
        compileLogExistsBefore = $compileBeforeExists
        compileLogExistsAfter = $compileAfterExists
        compileLogSha256Before = if ($compileBeforeExists) { [string]$CompileLogBefore.sha256 } else { $null }
        compileLogSizeBytesBefore = $compileBeforeSize
        compileLogSha256After = if ($compileAfterExists) { [string]$CompileLogAfter.sha256 } else { $null }
        compileLogSizeBytesAfter = $compileAfterSize
        compileLogProof = ($ProofResult -eq 'msvc-ltcg-compile-selector-verified')
        compileCommandHasGl = $compileHasGl
        compileCommandRustSelectorDefineCount = $compileRustSelectorCount
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
        [Parameter(Mandatory = $true)] [int]$Timeout,
        [Parameter(Mandatory = $true)] [object]$CompileLogBefore,
        [Parameter(Mandatory = $true)] [object]$CompileLogAfter,
        [Parameter(Mandatory = $true)] [string]$CompileLogPath
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
        $objectDump = Get-Content -LiteralPath $OutputPath -Raw
        $objectFormat = Assert-ProviderObjectFormat $objectDump $Configuration
        $symbols = @()
        $compileSelector = $null
        $verificationMethod = 'dumpbin-object-undefined'
        $verificationResult = 'dumpbin-unresolved-refs-verified'
        if ($Configuration -eq 'Release') {
            if (-not $CompileLogAfter.exists -or -not (Test-Sha256 $CompileLogAfter.sha256) -or
                [UInt64]$CompileLogAfter.sizeBytes -lt 1) {
                throw 'The Release provider compile log identity is invalid.'
            }
            $compileLogText = Get-Content -LiteralPath $CompileLogPath -Raw -ErrorAction Stop
            $compileLogReadAfter = Get-FileIdentity $CompileLogPath
            if ([string]$compileLogReadAfter.sha256 -cne [string]$CompileLogAfter.sha256 -or
                [UInt64]$compileLogReadAfter.sizeBytes -ne [UInt64]$CompileLogAfter.sizeBytes) {
                throw 'The Release provider compile log changed during selector proof.'
            }
            $compileSelector = Assert-ProviderCompileSelector $compileLogText 'OutputServiceRustProvider.cpp' $Backend
            $verificationMethod = 'msvc-ltcg-compile-selector'
            $verificationResult = 'msvc-ltcg-compile-selector-verified'
        }
        else {
            $symbols = @(Get-NormalizedProviderSymbols $objectDump)
        }
        $expected = New-Object Collections.Generic.List[string]
        foreach ($expectedSymbol in $script:OutputProviderSymbols) {
            [void]$expected.Add(([string]$expectedSymbol).ToLowerInvariant())
        }
        $expected.Sort([StringComparer]::Ordinal)
        if ($Configuration -ne 'Release' -and $Backend -eq 'rust') {
            if (($symbols -join '|') -cne ($expected.ToArray() -join '|')) {
                throw 'The Rust Output provider object does not reference the complete fixed v1 entrypoint set.'
            }
        }
        elseif ($Configuration -ne 'Release' -and $symbols.Count -ne 0) {
            throw 'The C++ Output provider object unexpectedly references Rust Output entrypoints.'
        }
        return New-SelectorProof $ProviderObjectBefore $ProviderObjectAfter $symbols $verificationResult $verificationMethod $objectFormat.format $CompileLogBefore $CompileLogAfter $compileSelector
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
        [Parameter(Mandatory = $true)] [object]$SelectorProof,
        [AllowNull()] [object]$FinalImageBinding = $null
    )
    $manifest = [ordered]@{
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
        providerObjectFormat = [string]$SelectorProof.providerObjectFormat
        verificationMethod = [string]$SelectorProof.verificationMethod
        compileLogExistsBefore = [bool]$SelectorProof.compileLogExistsBefore
        compileLogExistsAfter = [bool]$SelectorProof.compileLogExistsAfter
        compileLogSha256Before = $SelectorProof.compileLogSha256Before
        compileLogSizeBytesBefore = [UInt64]$SelectorProof.compileLogSizeBytesBefore
        compileLogSha256After = $SelectorProof.compileLogSha256After
        compileLogSizeBytesAfter = [UInt64]$SelectorProof.compileLogSizeBytesAfter
        compileLogProof = [bool]$SelectorProof.compileLogProof
        compileCommandHasGl = [bool]$SelectorProof.compileCommandHasGl
        compileCommandRustSelectorDefineCount = [int]$SelectorProof.compileCommandRustSelectorDefineCount
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
    if ($null -ne $FinalImageBinding) {
        $manifest.qualifiedFinalImage = $true
        $manifest.buildTarget = 'Rebuild'
        $manifest.qualification = 'qualified'
        $manifest.boundNativeEvidenceSha256 = [string]$FinalImageBinding.boundNativeEvidenceSha256
        $manifest.sourceNativeEvidenceSha256 = [string]$FinalImageBinding.sourceNativeEvidenceSha256
        $manifest.nativeEvidenceSha256 = [string]$FinalImageBinding.boundNativeEvidenceSha256
        $manifest.nativeEvidenceSourceSha256 = [string]$FinalImageBinding.sourceNativeEvidenceSha256
        $manifest.finalImageStage = [ordered]@{
            record = 'output-final-image-stage'
            stageId = [string]$FinalImageBinding.stageId
            receiptPath = [string]$FinalImageBinding.receiptPath
            receipt = [string]$FinalImageBinding.receiptPath
            receiptSha256 = [string]$FinalImageBinding.receiptSha256
            sourceNativeEvidenceSha256 = [string]$FinalImageBinding.sourceNativeEvidenceSha256
            exeSha256 = [string]$FinalImageBinding.files.exe.sha256
            exeSizeBytes = [UInt64]$FinalImageBinding.files.exe.sizeBytes
            mapSha256 = [string]$FinalImageBinding.files.map.sha256
            mapSizeBytes = [UInt64]$FinalImageBinding.files.map.sizeBytes
            provider = [ordered]@{
                memberCount = [int]$FinalImageBinding.provider.memberCount
                symbolCount = [int]$FinalImageBinding.provider.symbolCount
                mapSha256 = [string]$FinalImageBinding.provider.mapSha256
                mapSizeBytes = [UInt64]$FinalImageBinding.provider.mapSizeBytes
            }
        }
        $manifest.finalImageStageId = [string]$FinalImageBinding.stageId
        $manifest.finalImageReceiptPath = [string]$FinalImageBinding.receiptPath
        $manifest.finalImageReceiptSha256 = [string]$FinalImageBinding.receiptSha256
        $manifest.finalImageExeSha256 = [string]$FinalImageBinding.files.exe.sha256
        $manifest.finalImageExeSizeBytes = [UInt64]$FinalImageBinding.files.exe.sizeBytes
        $manifest.finalImageMapSha256 = [string]$FinalImageBinding.files.map.sha256
        $manifest.finalImageMapSizeBytes = [UInt64]$FinalImageBinding.files.map.sizeBytes
        $manifest.finalImageProvider = [ordered]@{
            memberCount = [int]$FinalImageBinding.provider.memberCount
            symbolCount = [int]$FinalImageBinding.provider.symbolCount
            mapSha256 = [string]$FinalImageBinding.provider.mapSha256
            mapSizeBytes = [UInt64]$FinalImageBinding.provider.mapSizeBytes
        }
    }
    else {
        $manifest.qualifiedFinalImage = $false
        $manifest.buildTarget = 'Build'
        $manifest.qualification = 'non-qualified'
    }
    return $manifest
}

function Assert-BuildManifest {
    param(
        [Parameter(Mandatory = $true)] [string]$Path,
        [Parameter(Mandatory = $true)] [object]$ExpectedSource,
        [Parameter(Mandatory = $true)] [object]$ExpectedArtifact,
        [Parameter(Mandatory = $true)] [object]$ExpectedStage,
        [Parameter(Mandatory = $true)] [object]$ExpectedSelectorProof,
        [AllowNull()] [object]$ExpectedFinalImageBinding = $null
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
    if ($null -ne $ExpectedFinalImageBinding) {
        $manifestStatusLineCount = Get-PropertyValue $manifest @('sourceStatusLineCount')
        if ($manifest.sourceDirty -isnot [bool] -or
            [bool]$manifest.sourceDirty -or
            $null -eq $manifestStatusLineCount -or
            $manifestStatusLineCount -is [bool] -or
            [string]$manifestStatusLineCount -notmatch '^[0-9]+$' -or
            [int]$manifestStatusLineCount -ne 0) {
            throw 'The qualified build manifest does not bind a clean source state.'
        }
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
    $expectedObjectFormat = [string](Get-PropertyValue $ExpectedSelectorProof @('providerObjectFormat'))
    $actualObjectFormat = [string](Get-PropertyValue $selectorProof @('providerObjectFormat'))
    $expectedVerificationMethod = [string](Get-PropertyValue $ExpectedSelectorProof @('verificationMethod'))
    $actualVerificationMethod = [string](Get-PropertyValue $selectorProof @('verificationMethod'))
    $expectedCompileProof = [bool](Get-PropertyValue $ExpectedSelectorProof @('compileLogProof'))
    $actualCompileProof = [bool](Get-PropertyValue $selectorProof @('compileLogProof'))
    $expectedCompileHasGl = [bool](Get-PropertyValue $ExpectedSelectorProof @('compileCommandHasGl'))
    $actualCompileHasGl = [bool](Get-PropertyValue $selectorProof @('compileCommandHasGl'))
    $expectedCompileSelectorCount = [int](Get-PropertyValue $ExpectedSelectorProof @('compileCommandRustSelectorDefineCount'))
    $actualCompileSelectorCount = [int](Get-PropertyValue $selectorProof @('compileCommandRustSelectorDefineCount'))
    $expectedCompileBeforeSize = [UInt64](Get-PropertyValue $ExpectedSelectorProof @('compileLogSizeBytesBefore'))
    $actualCompileBeforeSize = [UInt64](Get-PropertyValue $selectorProof @('compileLogSizeBytesBefore'))
    $expectedCompileAfterSize = [UInt64](Get-PropertyValue $ExpectedSelectorProof @('compileLogSizeBytesAfter'))
    $actualCompileAfterSize = [UInt64](Get-PropertyValue $selectorProof @('compileLogSizeBytesAfter'))
    if ($actualObjectFormat -cne $expectedObjectFormat -or
        $actualVerificationMethod -cne $expectedVerificationMethod -or
        $actualCompileProof -ne $expectedCompileProof -or
        $actualCompileHasGl -ne $expectedCompileHasGl -or
        $actualCompileSelectorCount -ne $expectedCompileSelectorCount -or
        $actualCompileBeforeSize -ne $expectedCompileBeforeSize -or
        $actualCompileAfterSize -ne $expectedCompileAfterSize -or
        [bool](Get-PropertyValue $selectorProof @('compileLogExistsBefore')) -ne [bool](Get-PropertyValue $ExpectedSelectorProof @('compileLogExistsBefore')) -or
        [bool](Get-PropertyValue $selectorProof @('compileLogExistsAfter')) -ne [bool](Get-PropertyValue $ExpectedSelectorProof @('compileLogExistsAfter')) -or
        [string](Get-PropertyValue $selectorProof @('compileLogSha256Before')) -cne [string](Get-PropertyValue $ExpectedSelectorProof @('compileLogSha256Before')) -or
        [string](Get-PropertyValue $selectorProof @('compileLogSha256After')) -cne [string](Get-PropertyValue $ExpectedSelectorProof @('compileLogSha256After'))) {
        throw 'The generated build manifest selector format or compile proof is stale.'
    }
    if ([string](Get-PropertyValue $manifest @('providerObjectFormat')) -cne $actualObjectFormat -or
        [string](Get-PropertyValue $manifest @('verificationMethod')) -cne $actualVerificationMethod -or
        [bool](Get-PropertyValue $manifest @('compileLogExistsBefore')) -ne [bool](Get-PropertyValue $selectorProof @('compileLogExistsBefore')) -or
        [bool](Get-PropertyValue $manifest @('compileLogExistsAfter')) -ne [bool](Get-PropertyValue $selectorProof @('compileLogExistsAfter')) -or
        [string](Get-PropertyValue $manifest @('compileLogSha256Before')) -cne [string](Get-PropertyValue $selectorProof @('compileLogSha256Before')) -or
        [UInt64](Get-PropertyValue $manifest @('compileLogSizeBytesBefore')) -ne $actualCompileBeforeSize -or
        [string](Get-PropertyValue $manifest @('compileLogSha256After')) -cne [string](Get-PropertyValue $selectorProof @('compileLogSha256After')) -or
        [UInt64](Get-PropertyValue $manifest @('compileLogSizeBytesAfter')) -ne $actualCompileAfterSize -or
        [bool](Get-PropertyValue $manifest @('compileLogProof')) -ne $actualCompileProof -or
        [bool](Get-PropertyValue $manifest @('compileCommandHasGl')) -ne $actualCompileHasGl -or
        [int](Get-PropertyValue $manifest @('compileCommandRustSelectorDefineCount')) -ne $actualCompileSelectorCount) {
        throw 'The generated build manifest top-level selector proof fields are stale.'
    }
    if ($Configuration -eq 'Release') {
        if ($expectedProofResult -cne 'msvc-ltcg-compile-selector-verified' -or
            $expectedObjectFormat -cne 'msvc-ltcg-anonymous' -or
            -not $expectedCompileProof -or -not $expectedCompileHasGl -or
            $expectedCompileAfterSize -lt 1 -or
            ($Backend -eq 'rust' -and $expectedCompileSelectorCount -ne 1) -or
            ($Backend -eq 'cpp' -and $expectedCompileSelectorCount -ne 0)) {
            throw 'The Release build manifest lacks the required LTCG selector proof.'
        }
    }
    else {
        if ($expectedProofResult -cne 'dumpbin-unresolved-refs-verified' -or
            $expectedObjectFormat -eq 'msvc-ltcg-anonymous' -or $expectedCompileProof -or
            $actualCompileHasGl -or $actualCompileSelectorCount -ne 0) {
            throw 'The Debug build manifest has an invalid selector proof format.'
        }
    }
    $selectorHash = Get-PropertyValue $manifest @('selectorProofSha256')
    if (-not (Test-Sha256 $selectorHash) -or [string]$selectorHash -ne [string]$ExpectedSelectorProof.selectorContractSha256) {
        throw 'The generated build manifest selector proof hash is invalid.'
    }
    $qualified = Get-PropertyValue $manifest @('qualifiedFinalImage')
    if ($qualified -isnot [bool]) { throw 'The generated build manifest qualified marker is invalid.' }
    if ($null -ne $ExpectedFinalImageBinding) {
        if (-not [bool]$qualified -or
            [string](Get-PropertyValue $manifest @('buildTarget')) -cne 'Rebuild' -or
            [string](Get-PropertyValue $manifest @('qualification')) -cne 'qualified') {
            throw 'The generated build manifest is not a qualified Rebuild.'
        }
        $expectedBoundHash = [string](Get-PropertyValue $ExpectedFinalImageBinding @('boundNativeEvidenceSha256'))
        $expectedSourceHash = [string](Get-PropertyValue $ExpectedFinalImageBinding @('sourceNativeEvidenceSha256'))
        if ([string](Get-PropertyValue $manifest @('boundNativeEvidenceSha256')) -cne $expectedBoundHash -or
            [string](Get-PropertyValue $manifest @('sourceNativeEvidenceSha256')) -cne $expectedSourceHash -or
            [string](Get-PropertyValue $manifest @('nativeEvidenceSha256')) -cne $expectedBoundHash -or
            [string](Get-PropertyValue $manifest @('nativeEvidenceSourceSha256')) -cne $expectedSourceHash) {
            throw 'The generated build manifest native evidence binding is stale.'
        }
        $expectedStageId = [string](Get-PropertyValue $ExpectedFinalImageBinding @('stageId'))
        $expectedReceiptPath = [string](Get-PropertyValue $ExpectedFinalImageBinding @('receiptPath'))
        $expectedReceiptHash = [string](Get-PropertyValue $ExpectedFinalImageBinding @('receiptSha256'))
        $expectedFiles = Get-PropertyValue $ExpectedFinalImageBinding @('files')
        $expectedExe = Get-PropertyValue $expectedFiles @('exe')
        $expectedMap = Get-PropertyValue $expectedFiles @('map')
        $expectedExeHash = [string](Get-PropertyValue $expectedExe @('sha256'))
        $expectedExeSize = [UInt64](Get-PropertyValue $expectedExe @('sizeBytes'))
        $expectedMapHash = [string](Get-PropertyValue $expectedMap @('sha256'))
        $expectedMapSize = [UInt64](Get-PropertyValue $expectedMap @('sizeBytes'))
        $expectedProvider = Get-PropertyValue $ExpectedFinalImageBinding @('provider')
        $manifestStage = Get-PropertyValue $manifest @('finalImageStage')
        $manifestStageKeys = if ($null -ne $manifestStage) { @($manifestStage.PSObject.Properties | ForEach-Object { $_.Name }) } else { @() }
        $expectedManifestStageKeys = @('exeSha256', 'exeSizeBytes', 'mapSha256', 'mapSizeBytes', 'provider', 'receipt', 'receiptPath', 'receiptSha256', 'record', 'sourceNativeEvidenceSha256', 'stageId')
        [Array]::Sort($manifestStageKeys, [StringComparer]::Ordinal)
        [Array]::Sort($expectedManifestStageKeys, [StringComparer]::Ordinal)
        if ($null -eq $manifestStage -or
            ($manifestStageKeys -join '|') -cne ($expectedManifestStageKeys -join '|') -or
            [string](Get-PropertyValue $manifestStage @('record')) -cne 'output-final-image-stage' -or
            [string](Get-PropertyValue $manifestStage @('stageId')) -cne $expectedStageId -or
            [string](Get-PropertyValue $manifestStage @('receiptPath')) -cne $expectedReceiptPath -or
            [string](Get-PropertyValue $manifestStage @('receipt')) -cne $expectedReceiptPath -or
            [string](Get-PropertyValue $manifestStage @('receiptSha256')) -cne $expectedReceiptHash -or
            [string](Get-PropertyValue $manifestStage @('sourceNativeEvidenceSha256')) -cne $expectedSourceHash -or
            [string](Get-PropertyValue $manifestStage @('exeSha256')) -cne $expectedExeHash -or
            [UInt64](Get-PropertyValue $manifestStage @('exeSizeBytes')) -ne $expectedExeSize -or
            [string](Get-PropertyValue $manifestStage @('mapSha256')) -cne $expectedMapHash -or
            [UInt64](Get-PropertyValue $manifestStage @('mapSizeBytes')) -ne $expectedMapSize) {
            throw 'The qualified final-image receipt binding is stale.'
        }
        $manifestStageProvider = Get-PropertyValue $manifestStage @('provider')
        $manifestTopProvider = Get-PropertyValue $manifest @('finalImageProvider')
        $expectedProviderKeys = @('mapSha256', 'mapSizeBytes', 'memberCount', 'symbolCount')
        foreach ($providerObject in @($manifestStageProvider, $manifestTopProvider)) {
            if ($null -eq $providerObject) { throw 'The qualified final-image provider identity is missing.' }
            $providerKeys = @($providerObject.PSObject.Properties | ForEach-Object { $_.Name })
            [Array]::Sort($providerKeys, [StringComparer]::Ordinal)
            [Array]::Sort($expectedProviderKeys, [StringComparer]::Ordinal)
            if (($providerKeys -join '|') -cne ($expectedProviderKeys -join '|')) {
                throw 'The qualified final-image provider schema is not exact.'
            }
        }
        foreach ($providerObject in @($manifestStageProvider, $manifestTopProvider)) {
            if ([int](Get-PropertyValue $providerObject @('memberCount')) -ne [int](Get-PropertyValue $expectedProvider @('memberCount')) -or
                [int](Get-PropertyValue $providerObject @('symbolCount')) -ne [int](Get-PropertyValue $expectedProvider @('symbolCount')) -or
                [string](Get-PropertyValue $providerObject @('mapSha256')) -cne [string](Get-PropertyValue $expectedProvider @('mapSha256')) -or
                [UInt64](Get-PropertyValue $providerObject @('mapSizeBytes')) -ne [UInt64](Get-PropertyValue $expectedProvider @('mapSizeBytes'))) {
                throw 'The qualified final-image provider identity is stale.'
            }
        }
        $expectedExeBareHash = $expectedExeHash -replace '^(?i:sha256:)', ''
        if ([string](Get-PropertyValue $manifest @('finalImageStageId')) -cne $expectedStageId -or
            [string](Get-PropertyValue $manifest @('finalImageReceiptPath')) -cne $expectedReceiptPath -or
            [string](Get-PropertyValue $manifest @('finalImageReceiptSha256')) -cne $expectedReceiptHash -or
            [string](Get-PropertyValue $manifest @('finalImageExeSha256')) -cne $expectedExeHash -or
            [UInt64](Get-PropertyValue $manifest @('finalImageExeSizeBytes')) -ne $expectedExeSize -or
            [string](Get-PropertyValue $manifest @('finalImageMapSha256')) -cne $expectedMapHash -or
            [UInt64](Get-PropertyValue $manifest @('finalImageMapSizeBytes')) -ne $expectedMapSize -or
            [string]$manifest.exeSha256 -cne $expectedExeBareHash -or
            [UInt64]$manifest.artifactSizeBytesAfter -ne $expectedExeSize) {
            throw 'The qualified final-image executable identity is not bound to the manifest.'
        }
    }
    elseif ([bool]$qualified) {
        throw 'The generated build manifest claims qualified mode without a final-image binding.'
    }
    else {
        if ([string](Get-PropertyValue $manifest @('buildTarget')) -cne 'Build' -or
            [string](Get-PropertyValue $manifest @('qualification')) -cne 'non-qualified') {
            throw 'The generated non-qualified build manifest marker is not explicit.'
        }
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
        [AllowNull()] [object]$Cleanup = $null,
        [AllowNull()] [string]$FailureCode = $null,
        [AllowNull()] [string]$FailureSubstage = $null
    )
    $failure = [ordered]@{
        stage = $script:Stage
        type = $Type
        adoption = 'HOLD'
        decision = 'HOLD'
        adoptionEligible = $false
    }
    if (-not [string]::IsNullOrWhiteSpace($FailureCode)) { $failure.code = $FailureCode }
    if (-not [string]::IsNullOrWhiteSpace($FailureSubstage)) { $failure.substage = $FailureSubstage }
    if ($Type -eq 'cleanup-unverified') {
        $failure.primaryStage = if ($null -ne $PrimaryStage) { $PrimaryStage } else { $script:Stage }
        $failure.primaryType = if ($null -ne $PrimaryType) { $PrimaryType } else { 'unknown' }
        if (-not [string]::IsNullOrWhiteSpace($FailureCode)) { $failure.primaryCode = $FailureCode }
        if (-not [string]::IsNullOrWhiteSpace($FailureSubstage)) { $failure.primarySubstage = $FailureSubstage }
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
    $selfTestConfiguration = $Configuration
    $Configuration = 'Debug'
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
        $temporaryIdentityVerified = $false
        $temporaryIdentityMismatchRejected = $false
        if ([Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT) {
            $identitySourcePath = Join-Path $root 'identity-source.tmp'
            $identityReplacementPath = Join-Path $root 'identity-replacement.tmp'
            $identityStream = $null
            try {
                $identityStream = [IO.FileStream]::new($identitySourcePath, [IO.FileMode]::CreateNew, [IO.FileAccess]::ReadWrite, [IO.FileShare]::ReadWrite, 4096, [IO.FileOptions]::SequentialScan)
                $identityRecord = Get-OwnedFileIdentity $identityStream
            }
            finally {
                if ($null -ne $identityStream) { $identityStream.Dispose(); $identityStream = $null }
            }
            [IO.File]::WriteAllText($identityReplacementPath, 'replacement')
            $replacementStream = $null
            try {
                $replacementStream = [IO.File]::Open($identityReplacementPath, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
                try { [void](Assert-OwnedFileIdentity $replacementStream $identityRecord 'self-test') }
                catch { $temporaryIdentityMismatchRejected = $true }
            }
            finally {
                if ($null -ne $replacementStream) { $replacementStream.Dispose(); $replacementStream = $null }
            }
            if (-not $temporaryIdentityMismatchRejected) { throw 'Self-test accepted a replacement final-image verification file identity.' }
            [void](Remove-VerifiedOwnedOutput $identitySourcePath $identityRecord 'self-test')
            Assert-RegularFile $identityReplacementPath
            [IO.File]::Delete($identityReplacementPath)
            $temporaryIdentityVerified = $true
        }
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
        $qualifiedDirtySourceRejected = $false
        $dirtySourceState = [pscustomobject][ordered]@{ head = ('0' * 40); dirty = $true; statusSha256 = ('1' * 64); statusLineCount = 0 }
        try { Assert-QualifiedSourceState $dirtySourceState }
        catch { $qualifiedDirtySourceRejected = $true }
        if (-not $qualifiedDirtySourceRejected) { throw 'Self-test accepted a dirty source state for qualified production.' }
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
        $selectorProof = New-SelectorProof ([pscustomobject]@{ exists = $false; sha256 = $null; sizeBytes = [UInt64]0 }) $artifact @() 'dumpbin-unresolved-refs-verified' 'dumpbin-object-undefined' 'coff-symbols'
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
        $rustLtcgSelectorVerified = $false
        $cppLtcgSelectorVerified = $false
        $missingGlRejected = $false
        $duplicateSelectorRejected = $false
        $ambiguousSourceRejected = $false
        $wrongConfigObjectFormatRejected = $false
        $syntheticProviderSource = '^C:\build\OutputServiceRustProvider.cpp'
        $rustCompileLog = $syntheticProviderSource + "`r`n" + 'cl.exe /nologo /c /GL /D SAKURA_OUTPUT_BACKEND_RUST /FoOutputServiceRustProvider.obj'
        $cppCompileLog = $syntheticProviderSource + "`r`n" + 'cl.exe /nologo /c /GL /FoOutputServiceRustProvider.obj'
        $missingGlCompileLog = $syntheticProviderSource + "`r`n" + 'cl.exe /nologo /c /D SAKURA_OUTPUT_BACKEND_RUST /FoOutputServiceRustProvider.obj'
        $duplicateSelectorCompileLog = $syntheticProviderSource + "`r`n" + 'cl.exe /nologo /c /GL /D SAKURA_OUTPUT_BACKEND_RUST /D SAKURA_OUTPUT_BACKEND_RUST /FoOutputServiceRustProvider.obj'
        $ambiguousSourceCompileLog = $rustCompileLog + "`r`n" + $rustCompileLog
        $anonymousObjectDump = 'File Type: ANONYMOUS OBJECT'
        $coffObjectDump = 'File Type: COFF OBJECT'
        $rustSelector = Assert-ProviderCompileSelector $rustCompileLog 'OutputServiceRustProvider.cpp' 'rust'
        if (-not $rustSelector.hasGl -or $rustSelector.rustSelectorDefineCount -ne 1) { throw 'Self-test Rust LTCG selector proof failed.' }
        $rustLtcgSelectorVerified = $true
        $cppSelector = Assert-ProviderCompileSelector $cppCompileLog 'OutputServiceRustProvider.cpp' 'cpp'
        if (-not $cppSelector.hasGl -or $cppSelector.rustSelectorDefineCount -ne 0) { throw 'Self-test C++ LTCG selector proof failed.' }
        $cppLtcgSelectorVerified = $true
        try { [void](Get-ProviderCompileSelector $missingGlCompileLog 'OutputServiceRustProvider.cpp') }
        catch { $missingGlRejected = $true }
        if (-not $missingGlRejected) { throw 'Self-test accepted a compile command without /GL.' }
        try { [void](Assert-ProviderCompileSelector $duplicateSelectorCompileLog 'OutputServiceRustProvider.cpp' 'rust') }
        catch { $duplicateSelectorRejected = $true }
        if (-not $duplicateSelectorRejected) { throw 'Self-test accepted duplicate Rust selector defines.' }
        try { [void](Get-ProviderCompileSelector $ambiguousSourceCompileLog 'OutputServiceRustProvider.cpp') }
        catch { $ambiguousSourceRejected = $true }
        if (-not $ambiguousSourceRejected) { throw 'Self-test accepted ambiguous provider source commands.' }
        $formatFailures = 0
        try { [void](Assert-ProviderObjectFormat $coffObjectDump 'Release') } catch { $formatFailures++ }
        try { [void](Assert-ProviderObjectFormat $anonymousObjectDump 'Debug') } catch { $formatFailures++ }
        if ($formatFailures -ne 2) { throw 'Self-test accepted a provider object format for the wrong configuration.' }
        $wrongConfigObjectFormatRejected = $true
        $nativeExitCodeCaptureVerified = $false
        $rustToolchainExitZeroVerified = $false
        $rustToolchainNonzeroRejected = $false
        $rustToolchainMalformedRejected = $false
        $rustToolchainFailureCodeVerified = $false
        $comspec = $null
        if ([Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT) {
            $comspec = Resolve-Executable 'cmd.exe'
            $zeroCapture = Invoke-NativeOutputCapture $comspec @('/d', '/s', '/c', 'exit 0') 4 256
            if ($zeroCapture.exitCode -ne 0 -or -not $zeroCapture.bounded -or $zeroCapture.lineCount -ne 0) {
                throw 'Self-test failed to preserve a native zero exit code.'
            }
            $nonzeroCapture = Invoke-NativeOutputCapture $comspec @('/d', '/s', '/c', 'exit 7') 4 256
            if ($nonzeroCapture.exitCode -ne 7) { throw 'Self-test failed to preserve a native nonzero exit code.' }
            $multilineCapture = Invoke-NativeOutputCapture $comspec @('/d', '/s', '/c', 'echo rustc-one&echo rustc-two') 4 256
            if ($multilineCapture.lineCount -ne 2) { throw 'Self-test did not retain native multiline output for validation.' }
            $tooManyCapture = Invoke-NativeOutputCapture $comspec @('/d', '/s', '/c', 'for /L %i in (1,1,5) do @echo rustc-line') 4 256
            if ($tooManyCapture.bounded) { throw 'Self-test accepted native output beyond its bound.' }
            $nativeExitCodeCaptureVerified = $true

            $rustcSelfTest = Resolve-RustcExecutable
            $rustcVersionCapture = Invoke-NativeOutputCapture $rustcSelfTest @('--version') 4 512
            if ($rustcVersionCapture.exitCode -ne 0) { throw 'Self-test Rust compiler returned a nonzero exit code.' }
            if ([string]::IsNullOrWhiteSpace((Convert-RustToolchainCaptureToIdentity $rustcVersionCapture))) {
                throw 'Self-test Rust compiler identity was empty.'
            }
            $rustToolchainExitZeroVerified = $true

            $successCapture = [pscustomobject][ordered]@{
                exitCode = 0
                lines = @('rustc 1.96.0 (self-test)')
                lineCount = 1
                bounded = $true
            }
            if ((Convert-RustToolchainCaptureToIdentity $successCapture) -cne 'rustc 1.96.0 (self-test)') {
                throw 'Self-test Rust toolchain success conversion failed.'
            }
            try { [void](Convert-RustToolchainCaptureToIdentity $nonzeroCapture) }
            catch {
                if ($script:FailureCode -cne 'RUST_TOOLCHAIN_IDENTITY_COMMAND_FAILED' -or
                    $script:FailureSubstage -cne 'rust-toolchain-identity') { throw 'Self-test Rust nonzero failure code was unstable.' }
                $rustToolchainNonzeroRejected = $true
            }
            if (-not $rustToolchainNonzeroRejected) { throw 'Self-test accepted a failed Rust toolchain command.' }
            try { [void](Convert-RustToolchainCaptureToIdentity $multilineCapture) }
            catch {
                if ($script:FailureCode -cne 'RUST_TOOLCHAIN_IDENTITY_MALFORMED' -or
                    $script:FailureSubstage -cne 'rust-toolchain-identity') { throw 'Self-test Rust malformed failure code was unstable.' }
                $rustToolchainMalformedRejected = $true
            }
            if (-not $rustToolchainMalformedRejected) { throw 'Self-test accepted multiline Rust toolchain output.' }
            $typedEnvelope = New-FailureEnvelope 'preflight' 'source-state' 'preflight' $null 'RUST_TOOLCHAIN_IDENTITY_MALFORMED' 'rust-toolchain-identity'
            $typedFailure = Get-PropertyValue $typedEnvelope @('failure')
            if ([string](Get-PropertyValue $typedFailure @('code')) -cne 'RUST_TOOLCHAIN_IDENTITY_MALFORMED' -or
                [string](Get-PropertyValue $typedFailure @('substage')) -cne 'rust-toolchain-identity') {
                throw 'Self-test typed Rust failure envelope failed.'
            }
            $rustToolchainFailureCodeVerified = $true
        }
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
        $cleanupEnvelope = New-FailureEnvelope 'cleanup-unverified' 'self-test' 'synthetic-primary' $syntheticCleanup 'SELF_TEST_PRIMARY_FAILURE' 'self-test-primary'
        if ([string](Get-PropertyValue $cleanupEnvelope @('status')) -cne 'failed' -or
            [string](Get-PropertyValue (Get-PropertyValue $cleanupEnvelope @('failure')) @('type')) -cne 'cleanup-unverified' -or
            [string](Get-PropertyValue (Get-PropertyValue $cleanupEnvelope @('failure')) @('primaryType')) -cne 'synthetic-primary' -or
            [string](Get-PropertyValue (Get-PropertyValue $cleanupEnvelope @('failure')) @('primaryCode')) -cne 'SELF_TEST_PRIMARY_FAILURE' -or
            [int](Get-PropertyValue (Get-PropertyValue $cleanupEnvelope @('cleanup')) @('remainingCount')) -ne 2) {
            throw 'Self-test cleanup failure envelope semantics failed.'
        }
        [void](Assert-PayloadFreeManifest $cleanupEnvelope)
        $cleanupEnvelopeVerified = $true
        $jobVerified = $false
        if ([Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT) {
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
            qualifiedDirtySourceRejected = $qualifiedDirtySourceRejected
            temporaryIdentityVerified = $temporaryIdentityVerified
            temporaryIdentityMismatchRejected = $temporaryIdentityMismatchRejected
            rustSelectorProofVerified = $rustSelectorProofVerified
            rustLtcgSelectorVerified = $rustLtcgSelectorVerified
            cppLtcgSelectorVerified = $cppLtcgSelectorVerified
            missingGlRejected = $missingGlRejected
            duplicateSelectorRejected = $duplicateSelectorRejected
            ambiguousSourceRejected = $ambiguousSourceRejected
            wrongConfigObjectFormatRejected = $wrongConfigObjectFormatRejected
            archiveExportsVerified = $true
            archiveExactSetRejected = $archiveExactSetRejected
            cleanupEnvelopeVerified = $cleanupEnvelopeVerified
            nativeExitCodeCaptureVerified = $nativeExitCodeCaptureVerified
            rustToolchainExitZeroVerified = $rustToolchainExitZeroVerified
            rustToolchainNonzeroRejected = $rustToolchainNonzeroRejected
            rustToolchainMalformedRejected = $rustToolchainMalformedRejected
            rustToolchainFailureCodeVerified = $rustToolchainFailureCodeVerified
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
        $Configuration = $selfTestConfiguration
    }
}

function Invoke-Producer {
    $transactionRoot = $null
    $finalRoot = $null
    $finalImageStageRootPath = $null
    $finalImageStageRootOwned = $false
    $published = $false
    $movedFinalRoot = $false
    $failureType = $null
    $failureStage = $null
    $failureCode = $null
    $failureSubstage = $null
    $successSummary = $null
    $successJson = $null
    $cleanupVerified = $true
    $cleanupFailureCount = 0
    try {
        $script:Stage = 'preflight'
        Set-ProducerFailureContext 'PRODUCER_PREFLIGHT' 'preflight'
        Assert-BackendSelector
        if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) { throw 'The producer requires Windows.' }
        $outputRoot = Get-OutputRoot $OutputDirectory
        $configurationRoot = Join-Path $outputRoot $Configuration
        Assert-NoReparseAncestors $configurationRoot
        [void][IO.Directory]::CreateDirectory($configurationRoot)
        Assert-RegularDirectory $configurationRoot
        $finalRoot = Join-Path $configurationRoot $Backend
        if (Test-Path -LiteralPath $finalRoot) { throw 'The selected output transaction already exists; refusing overwrite.' }
        if ($QualifiedFinalImage) {
            $finalImageStageRootPath = Get-OutputRoot $FinalImageStageRoot
            if ((Test-PathBelow $finalImageStageRootPath $configurationRoot) -or
                (Test-PathBelow $configurationRoot $finalImageStageRootPath) -or
                (Test-Path -LiteralPath $finalImageStageRootPath)) {
                throw 'The qualified final-image stage root must be a new directory outside the producer transaction root.'
            }
            [void][IO.Directory]::CreateDirectory($finalImageStageRootPath)
            $finalImageStageRootOwned = $true
            $script:FinalImageStageRoot = $finalImageStageRootPath
            $script:FinalImageStageRootOwned = $true
            Assert-RegularDirectory $finalImageStageRootPath
        }
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
        $compileLogSource = Join-Path $script:RepoRoot ('build/{0}/{1}/sakura_core/sakura.tlog/CL.command.1.tlog' -f $Platform, $Configuration)
        $rustProfile = if ($Configuration -eq 'Debug') { 'debug' } else { 'release' }
        $rustArchiveSource = Join-Path $script:RepoRoot ('build/{0}/{1}/rust/native/x86_64-pc-windows-msvc/{2}/sakura_native_ffi.lib' -f $Platform, $Configuration, $rustProfile)
        $canonicalStage = Join-Path $script:RepoRoot ('build/staging/{0}/sakura-editor' -f $context)
        $buildBatch = Join-Path $script:RepoRoot 'build-dev.bat'
        $buildScript = Join-Path $script:RepoRoot 'tools/build/sakura_build.py'
        if (-not $QualifiedFinalImage) { Assert-RegularFile $buildBatch }
        Assert-RegularFile $buildScript
        $script:Stage = 'source-state'
        Set-ProducerFailureContext 'SOURCE_STATE' 'source-state'
        Set-ProducerFailureContext 'SOURCE_GIT_STATE' 'git-state'
        $sourceBefore = Get-SourceState
        if ($QualifiedFinalImage) { Assert-QualifiedSourceState $sourceBefore }
        Set-ProducerFailureContext 'ARTIFACT_BEFORE_IDENTITY' 'artifact-before'
        $artifactBefore = Get-OptionalFileIdentity $artifactSource
        Set-ProducerFailureContext 'PROVIDER_OBJECT_BEFORE_IDENTITY' 'provider-object-before'
        $providerObjectBefore = Get-OptionalFileIdentity $providerObjectSource
        Set-ProducerFailureContext 'COMPILE_LOG_BEFORE_IDENTITY' 'compile-log-before'
        $compileLogBefore = Get-OptionalFileIdentity $compileLogSource
        Set-ProducerFailureContext 'WINDOWS_IMAGE_IDENTITY' 'windows-image'
        $windowsBefore = Get-WindowsImageIdentity
        Set-ProducerFailureContext 'POWER_MODE_IDENTITY' 'power-mode'
        $powerBefore = Get-PowerModeIdentity
        Set-ProducerFailureContext 'MSVC_TOOLCHAIN_IDENTITY' 'msvc-toolchain'
        $msvcIdentity = Get-MsvcIdentity
        Set-ProducerFailureContext 'RUST_TOOLCHAIN_IDENTITY' 'rust-toolchain-identity'
        $rustToolchain = Get-RustToolchainIdentity
        Set-ProducerFailureContext 'RUST_LOCK_IDENTITY' 'rust-lock'
        $lockPath = Join-Path $script:RepoRoot 'rust/native/Cargo.lock'
        $rustLockSha256 = Get-Sha256 $lockPath
        Set-ProducerFailureContext 'COMMAND_SHELL_RESOLUTION' 'command-shell'
        $comspec = Resolve-Executable 'cmd.exe'
        Set-ProducerFailureContext 'PYTHON_RESOLUTION' 'python'
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
        Set-ProducerFailureContext 'DUMPBIN_RESOLUTION' 'dumpbin'
        $dumpbinProbePath = Join-Path $transactionRoot 'dumpbin-path.txt'
        $dumpbin = Resolve-Dumpbin $comspec $script:RepoRoot $environmentBlock $TimeoutSeconds $dumpbinProbePath
        $packagePlanCommandText = '{0} -3 {1} --format json package plan sakura_app --context {2}' -f
            (ConvertTo-WindowsCommandLineArgument $py),
            (ConvertTo-WindowsCommandLineArgument $buildScript),
            $context
        $packagePlanPath = Join-Path $transactionRoot 'package-plan.json'
        $script:Stage = 'package-plan'
        Set-ProducerFailureContext 'PACKAGE_PLAN' 'package-plan'
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
        $sourceAfterPackagePlan = Get-SourceState
        Assert-SourceStateEqual $sourceBefore $sourceAfterPackagePlan
        if ($QualifiedFinalImage) { Assert-QualifiedSourceState $sourceAfterPackagePlan }
        $buildCommandDescriptor = if ($QualifiedFinalImage) {
            'inventory-observe-product|Rebuild|msvc-x64-{0}|sakura_app|output={1}|utf16=cpp|output-production=false|utf16-production=false|telemetry=false|listings=false|SKIP_CREATE_GITHASH=1|timeout={2}|package-timeout={3}|jobs={4}|MSBUILDDISABLENODEREUSE=1|final-image=required' -f
                $Configuration.ToLowerInvariant(), $Backend, $TimeoutSeconds, $PackageTimeoutSeconds, $BuildParallelism
        }
        else {
            'build-dev.bat|x64|{0}|SAKURA_OUTPUT_BACKEND={1}|SAKURA_UTF16_BACKEND=cpp|SAKURA_OUTPUT_PRODUCTION_PACKAGE=false|SAKURA_UTF16_PRODUCTION_PACKAGE=false|SAKURA_UTF16_BENCHMARK_TELEMETRY=false|SAKURA_GENERATE_ASSEMBLY_LISTINGS=false|SKIP_CREATE_GITHASH=1|SAKURA_BUILD_JOBS={2}|MSBUILDDISABLENODEREUSE=1' -f $Configuration, $Backend, $BuildParallelism
        }
        $buildCommandSha256 = Get-TextSha256 $buildCommandDescriptor
        $runtimeStageCommandSha256 = Get-TextSha256 ('stage-runtime|{0}|sakura_app|canonical' -f $context)
        $script:Stage = 'build'
        Set-ProducerFailureContext 'BUILD' 'build'
        $script:BuildStarted = $true
        $buildStartedUtc = [DateTime]::UtcNow
        $nativeEvidencePath = Join-Path $transactionRoot 'native-product.json'
        $observerOuterTimeoutSeconds = $TimeoutSeconds
        $buildCommandText = if ($QualifiedFinalImage) {
            $observerCommand = New-QualifiedObserverCommand $py $buildScript $context $BuildParallelism $TimeoutSeconds $PackageTimeoutSeconds $Backend $finalImageStageRootPath $nativeEvidencePath
            $observerCommand + ' > ' + (ConvertTo-WindowsCommandLineArgument (Join-Path $transactionRoot 'native-product.stdout')) +
                ' 2> ' + (ConvertTo-WindowsCommandLineArgument (Join-Path $transactionRoot 'native-product.stderr'))
            # observe-product runs its package closure before the Rebuild. The
            # outer owner therefore needs both bounded phases plus grace.
            $observerWorkTimeoutSeconds = [Int64]$TimeoutSeconds + [Int64]$PackageTimeoutSeconds
            $observerOuterTimeoutSeconds = [int][Math]::Min([Int64]::MaxValue, ($observerWorkTimeoutSeconds + 60))
        }
        else {
            '{0} x64 {1}' -f (ConvertTo-WindowsCommandLineArgument $buildBatch), $Configuration
        }
        [void](Invoke-OwnedCommand $comspec (New-CmdCommandLine $buildCommandText $comspec) $script:RepoRoot $environmentBlock $observerOuterTimeoutSeconds)
        $sourceAfterBuild = Get-SourceState
        Assert-SourceStateEqual $sourceBefore $sourceAfterBuild
        if ($QualifiedFinalImage) { Assert-QualifiedSourceState $sourceAfterBuild }
        $artifactAfter = Get-FileIdentity $artifactSource
        $finalImageBinding = $null
        if ($QualifiedFinalImage) {
            $finalImageBinding = Invoke-QualifiedFinalImageVerification $py $buildScript $nativeEvidencePath $finalImageStageRootPath $Backend $Platform $Configuration $artifactAfter $comspec $environmentBlock $TimeoutSeconds 'build-final-image-verify'
            $firstFinalImageBinding = $finalImageBinding
            [void](Assert-QualifiedFinalImageVerificationEqual $firstFinalImageBinding $finalImageBinding)
        }
        $providerObjectAfter = Get-FileIdentity $providerObjectSource
        $compileLogAfter = Get-OptionalFileIdentity $compileLogSource
        foreach ($nativeTemporary in @((Join-Path $transactionRoot 'native-product.stdout'), (Join-Path $transactionRoot 'native-product.stderr'))) {
            if (Test-Path -LiteralPath $nativeTemporary) { Assert-RegularFile $nativeTemporary; [IO.File]::Delete($nativeTemporary) }
        }
        if (-not $QualifiedFinalImage -and $Configuration -eq 'Release') {
            $providerObjectItem = Get-Item -LiteralPath $providerObjectSource -Force -ErrorAction Stop
            $compileLogItem = Get-Item -LiteralPath $compileLogSource -Force -ErrorAction Stop
            if ($providerObjectItem.LastWriteTimeUtc -le $buildStartedUtc -or
                $compileLogItem.LastWriteTimeUtc -le $buildStartedUtc) {
                throw 'Release provider object and compile log were not produced by this build.'
            }
        }
        $archiveProofOutput = Join-Path $transactionRoot 'archive-proof.txt'
        $selectorProofOutput = Join-Path $transactionRoot 'selector-proof.txt'
        $script:Stage = 'selector-proof'
        Set-ProducerFailureContext 'SELECTOR_PROOF' 'selector-proof'
        $archiveProof = Get-ProviderArchiveProof $rustArchiveSource $dumpbin $comspec $environmentBlock $script:RepoRoot $archiveProofOutput $TimeoutSeconds
        $selectorProof = Get-SelectorProof $providerObjectBefore $providerObjectAfter $providerObjectSource $dumpbin $comspec $environmentBlock $script:RepoRoot $selectorProofOutput $TimeoutSeconds $compileLogBefore $compileLogAfter $compileLogSource
        $selectorProof = Add-ProviderArchiveProof $selectorProof $archiveProof
        $expectedSelectorResult = if ($Configuration -eq 'Release') { 'msvc-ltcg-compile-selector-verified' } else { 'dumpbin-unresolved-refs-verified' }
        if ([string]$selectorProof.result -cne $expectedSelectorResult) { throw 'The Output selector proof did not use the expected configuration-specific verification.' }
        if ([UInt64]$artifactAfter.sizeBytes -lt 1) { throw 'The built sakura.exe is empty.' }
        $script:Stage = 'runtime-stage'
        Set-ProducerFailureContext 'RUNTIME_STAGE' 'runtime-stage'
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
        $manifestPath = Join-Path $transactionRoot 'build-manifest.json'
        $script:Stage = 'manifest'
        Set-ProducerFailureContext 'MANIFEST' 'manifest'
        if ($QualifiedFinalImage) {
            $manifestFinalImageBinding = Invoke-QualifiedFinalImageVerification $py $buildScript $nativeEvidencePath $finalImageStageRootPath $Backend $Platform $Configuration $artifactAfter $comspec $environmentBlock $TimeoutSeconds 'manifest-final-image-verify'
            [void](Assert-QualifiedFinalImageVerificationEqual $firstFinalImageBinding $manifestFinalImageBinding)
            $finalImageBinding = $manifestFinalImageBinding
        }
        $manifest = New-BuildManifest $sourceBefore $artifactBefore $artifactAfter $copiedStage $windowsBefore.identity $windowsBefore.sha256 $powerBefore.identity $powerBefore.sha256 $msvcIdentity $rustToolchain $rustLockSha256 $packagePlanValue $buildCommandSha256 $packagePlanCommandSha256 $runtimeStageCommandSha256 $selectorProof $finalImageBinding
        Write-JsonAtomic $manifestPath $manifest
        [void](Assert-BuildManifest $manifestPath $sourceBefore $artifactAfter $copiedStage $selectorProof $finalImageBinding)
        if ((Get-Sha256 $artifactSource) -ne [string]$artifactAfter.sha256) { throw 'The selected artifact changed after manifest generation.' }
        if (Test-Path -LiteralPath $finalRoot) { throw 'The selected output transaction appeared during production.' }
        $script:Stage = 'publication'
        Set-ProducerFailureContext 'PUBLICATION' 'publication'
        [IO.Directory]::Move($transactionRoot, $finalRoot)
        $movedFinalRoot = $true
        if ($QualifiedFinalImage) {
            $publishedNativeEvidencePath = Join-Path $finalRoot 'native-product.json'
            $publishedFinalImageBinding = Invoke-QualifiedFinalImageVerification $py $buildScript $publishedNativeEvidencePath $finalImageStageRootPath $Backend $Platform $Configuration $artifactAfter $comspec $environmentBlock $TimeoutSeconds 'publication-final-image-verify'
            [void](Assert-QualifiedFinalImageVerificationEqual $firstFinalImageBinding $publishedFinalImageBinding)
            $finalImageBinding = $publishedFinalImageBinding
        }
        $finalManifest = Join-Path $finalRoot 'build-manifest.json'
        $finalStage = Join-Path $finalRoot 'runtime-stage'
        $finalStageSnapshot = Get-RuntimeStageSnapshot $finalStage $context $artifactAfter
        [void](Assert-BuildManifest $finalManifest $sourceBefore $artifactAfter $finalStageSnapshot $selectorProof $finalImageBinding)
        if ((Get-Sha256 $artifactSource) -ne [string]$artifactAfter.sha256) { throw 'The selected artifact changed after publication.' }
        Assert-SourceStateEqual $sourceBefore (Get-SourceState)
        $summary = [ordered]@{
            schemaVersion = $script:SchemaVersion
            record = 'output-startup-build-producer'
            payloadFree = $true
            status = 'committed'
            backend = $Backend
            platform = $Platform.ToLowerInvariant()
            configuration = $Configuration
            qualifiedFinalImage = [bool]$QualifiedFinalImage
            buildTarget = if ($QualifiedFinalImage) { 'Rebuild' } else { 'Build' }
            qualification = if ($QualifiedFinalImage) { 'qualified' } else { 'non-qualified' }
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
            boundNativeEvidenceSha256 = if ($QualifiedFinalImage) { [string]$finalImageBinding.boundNativeEvidenceSha256 } else { $null }
            sourceNativeEvidenceSha256 = if ($QualifiedFinalImage) { [string]$finalImageBinding.sourceNativeEvidenceSha256 } else { $null }
            finalImageStageId = if ($QualifiedFinalImage) { [string]$finalImageBinding.stageId } else { $null }
            finalImageReceiptPath = if ($QualifiedFinalImage) { [string]$finalImageBinding.receiptPath } else { $null }
            finalImageReceiptSha256 = if ($QualifiedFinalImage) { [string]$finalImageBinding.receiptSha256 } else { $null }
            finalImageExeSha256 = if ($QualifiedFinalImage) { [string]$finalImageBinding.files.exe.sha256 } else { $null }
            finalImageExeSizeBytes = if ($QualifiedFinalImage) { [UInt64]$finalImageBinding.files.exe.sizeBytes } else { $null }
            finalImageMapSha256 = if ($QualifiedFinalImage) { [string]$finalImageBinding.files.map.sha256 } else { $null }
            finalImageMapSizeBytes = if ($QualifiedFinalImage) { [UInt64]$finalImageBinding.files.map.sizeBytes } else { $null }
            finalImageProvider = if ($QualifiedFinalImage) { [ordered]@{
                memberCount = [int]$finalImageBinding.provider.memberCount
                symbolCount = [int]$finalImageBinding.provider.symbolCount
                mapSha256 = [string]$finalImageBinding.provider.mapSha256
                mapSizeBytes = [UInt64]$finalImageBinding.provider.mapSizeBytes
            } } else { $null }
        }
        $successSummary = $summary
        [void](Assert-PayloadFreeManifest $summary)
        $successJson = $summary | ConvertTo-Json -Compress -Depth 10
        if ([string]::IsNullOrWhiteSpace($successJson)) { throw 'The producer success summary serialized to an empty payload.' }
        $published = $true
        $script:Published = $true
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
        $failureCode = $script:FailureCode
        $failureSubstage = $script:FailureSubstage
        $script:ProducerExitCode = 1
    }
    finally {
        if (-not $published) {
            if ($finalImageStageRootOwned -and $null -ne $finalImageStageRootPath) {
                try { [void](Remove-OwnedDirectory $finalImageStageRootPath) }
                catch { $cleanupVerified = $false; $cleanupFailureCount++ }
            }
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
    $finalImageStageRootExists = if ($null -ne $finalImageStageRootPath) { [bool](Test-Path -LiteralPath $finalImageStageRootPath) } else { $false }
    $lockExists = if ($null -ne $script:LockPath) { [bool](Test-Path -LiteralPath $script:LockPath) } else { $false }
    $unexpectedFinalRoot = (-not $published) -and $finalRootExists
    $unexpectedFinalImageStageRoot = (-not $published) -and $finalImageStageRootExists
    $remainingCount = 0
    if ($unexpectedFinalRoot) { $remainingCount++ }
    if ($unexpectedFinalImageStageRoot) { $remainingCount++ }
    if ($transactionRootExists) { $remainingCount++ }
    if ($lockExists) { $remainingCount++ }
    if ($remainingCount -ne 0) { $cleanupVerified = $false }
    $cleanup = [ordered]@{
        attempted = $true
        verified = [bool]$cleanupVerified
        finalRootExists = $finalRootExists
        finalRootExpected = [bool]$published
        finalImageStageRootExists = $finalImageStageRootExists
        finalImageStageRootExpected = [bool]$published
        transactionRootExists = $transactionRootExists
        lockExists = $lockExists
        remainingCount = [int]$remainingCount
        failureCount = [int]$cleanupFailureCount
    }
    if ($null -ne $failureType -or -not $cleanupVerified) {
        $envelopeType = if ($cleanupVerified) { $failureType } else { 'cleanup-unverified' }
        $envelopeFailureCode = if ($null -ne $failureType) { $failureCode } else { $null }
        $envelopeFailureSubstage = if ($null -ne $failureType) { $failureSubstage } else { $null }
        try {
            Write-Output ((New-FailureEnvelope $envelopeType $failureStage $failureType $cleanup $envelopeFailureCode $envelopeFailureSubstage) | ConvertTo-Json -Compress -Depth 10)
        }
        catch { }
        $script:ProducerExitCode = 1
        return
    }
    Write-Output $successJson
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
