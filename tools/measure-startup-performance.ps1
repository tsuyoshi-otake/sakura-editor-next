# Requires -Version 5.1
<#
.SYNOPSIS
Measures the time required for Sakura Editor to present a Markdown document.

.DESCRIPTION
Each measurement copies the supplied executable into a measurement-owned bundle,
writes the exact portable sakura.exe.ini sidecar, and uses a generated profile
below that bundle. The profile name, process identity records, and path checks
are deliberate: this script must never fall back to the user's profile, close
an already-running editor, or delete an arbitrary folder.
#>
[CmdletBinding()]
param(
    [string]$SakuraExe,
    [string]$SampleMarkdown = 'tools/startup-benchmark-sample.md',
    [ValidateRange(1, 100)]
    [int]$Iterations = 5,
    [switch]$CompareExistingProfile,
    [switch]$CaptureScreenshot,
    [switch]$MeasureDwmFlush,
    [string]$OutputDirectory = (Join-Path $HOME 'tmp\sakuracode-startup-performance'),
    [switch]$SelfTest,
    [switch]$LibraryOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$startupTimeoutMs = 30000
$closeTimeoutMs = 3000
$pollIntervalMs = 25
$startupDiagnosticCheckpointNames = @('0.5s', '2s', '10s', 'timeout')
$startupDiagnosticCheckpointMs = [ordered]@{
    '0.5s' = 500
    '2s' = 2000
    '10s' = 10000
    'timeout' = $startupTimeoutMs
}
$startupDiagnosticFailureSubstages = @(
    'none', 'root-exit', 'job-membership',
    'process-census-first', 'process-identity-first',
    'process-census-second', 'process-identity-second',
    'window-enumeration', 'projection-finalize'
)
$startupDiagnosticMaxProcessCount = 256
$startupDiagnosticMaxImageNameLength = 260
$startupDiagnosticMaxWindowCount = 1024
$startupProcessEnumerationMaxEntryCount = 65536
$startupProcessEnumerationMaxAttemptCount = 3
$startupJobQueryMaxAttemptCount = 8
$startupJobQueryMaxBytes = [UInt64]1048576
$startupJobQueryHeaderBytes = [UInt64]8
$startupJobQueryMaxProcessCount = [int](($startupJobQueryMaxBytes - $startupJobQueryHeaderBytes) / [UInt64]([IntPtr]::Size))
$startupJobQueryRetryableErrorCodes = [int[]]@(122, 24, 234)
$startupContainmentMaxOuterPolls = 8
$startupIdentityOperations = @('none', 'open-process', 'query-image-path', 'get-process-times', 'exception')
$startupIdentityObserverRoles = @('none', 'launch-job-member', 'graceful-job-member', 'post-close-tracked-history', 'exact-path')
$startupIdentityReconciliationOperations = @('open-process', 'query-image-path', 'get-process-times')
$startupIdentityReconciliationObserverRoles = @('post-close-tracked-history')
$startupIdentityReconciliationReasons = @(
    'none', 'job-empty-and-allowlisted-post-close-history', 'job-empty-not-proven',
    'operation-not-allowlisted', 'observer-not-allowlisted', 'exact-path-failure', 'malformed'
)
$startupContainmentModes = @('graceful-job-empty', 'explicit-job-termination', 'unavailable')
$startupContainmentTerminalStates = @(
    'not-attempted', 'verified-graceful-job-empty', 'verified-explicit-job-termination',
    'rejected-job-unavailable', 'rejected-termination-failed', 'rejected-terminal-job-query',
    'rejected-job-members-remain', 'rejected-job-close', 'rejected-post-close-observation', 'exception'
)
$startupObservationMaxCount = 4096
$startupTrackedSweepFailureTypes = @(
    'none', 'identity-disappeared', 'identity-still-present', 'enumeration-unavailable', 'identity-unavailable', 'exception'
)
$startupTrackedSweepCounterFields = @(
    'trackedSweepIdentityAttemptCount', 'trackedSweepIdentityFailureCount',
    'trackedSweepDisappearedAfterSnapshotCount', 'trackedSweepStillPresentAfterFailureCount',
    'trackedSweepPassCount'
)
$startupJobIdentityCounterFields = @(
    'identityAttemptCount', 'identityFailureCount', 'recoveryAttemptCount',
    'disappearedAfterSnapshotCount', 'stillPresentAfterFailureCount'
)
$startupAffinityFailureTypes = @(
    'none', 'open', 'set', 'readback', 'mismatch', 'identity', 'verification', 'unavailable'
)
$startupAffinityLiveSetSources = @(
    'not-attempted', 'process-snapshot', 'tracked-sweep', 'unavailable'
)

# Keep trace parsing bounded even when a run-owned directory is corrupted or
# an instrumented process writes unexpectedly large output.  The paired runner
# consumes these values through Get-StartupTraceBounds, so both scripts share
# one fixed contract without copying a second set of limits.
function Get-StartupTraceBounds {
    return [pscustomobject][ordered]@{
        maxFiles = 8
        maxBytes = [Int64]1048576
        maxLines = 4096
        maxValidRecords = 4096
        maxLineLength = 65536
    }
}

$startupProbeReferences = New-Object System.Collections.Generic.List[string]
try {
    $startupProbeRuntimeDirectory = [Runtime.InteropServices.RuntimeEnvironment]::GetRuntimeDirectory()
    $startupProbeDrawingCommon = Join-Path $startupProbeRuntimeDirectory 'System.Drawing.Common.dll'
    if ([IO.File]::Exists($startupProbeDrawingCommon)) {
        [void]$startupProbeReferences.Add($startupProbeDrawingCommon)
    }
    else {
        [void]$startupProbeReferences.Add('System.Drawing.dll')
    }
    foreach ($startupProbeReferenceName in @('System.dll', 'System.Collections.dll', 'System.Runtime.dll', 'System.Drawing.Primitives.dll', 'System.Private.Windows.Core.dll', 'System.Private.Windows.GdiPlus.dll')) {
        $startupProbeReference = Join-Path $startupProbeRuntimeDirectory $startupProbeReferenceName
        if ([IO.File]::Exists($startupProbeReference)) { [void]$startupProbeReferences.Add($startupProbeReference) }
    }
}
catch { [void]$startupProbeReferences.Add('System.Drawing.dll') }

Add-Type -TypeDefinition @'
using System;
using System.Collections;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Text;

public sealed class StartupProbeWindow
{
    public IntPtr Handle;
    public int ProcessId;
    public bool Visible;
    public string Caption;
    public string ClassName;
}

public sealed class StartupProbeProcess
{
    public int ProcessId;
    public int ParentProcessId;
    public long CreationTime;
    public string ImagePath;
}

public sealed class StartupProbeProcessEntry
{
    public int ProcessId;
    public int ParentProcessId;
    public string ImageName;
}

public sealed class StartupProbeProcessEntriesResult
{
    public bool Attempted;
    public bool Complete;
    public bool Succeeded;
    public int ErrorCode;
    public int AttemptCount;
    public int RetryCount;
    public bool Retried;
    public StartupProbeProcessEntry[] Entries;

    public StartupProbeProcessEntriesResult()
    {
        Entries = new StartupProbeProcessEntry[0];
    }
}

public sealed class StartupProbeAffinity
{
    public int ProcessId;
    public ulong RequestedMask;
    public ulong ProcessMask;
    public ulong SystemMask;
    public bool Opened;
    public bool SetSucceeded;
    public bool ReadBackSucceeded;
    public bool Verified;
    public bool DescendantsVerified;
    public int ErrorCode;
}

public sealed class StartupProbeProcessIdentityResult
{
    public bool Succeeded;
    public int ErrorCode;
    public string Operation;
    public StartupProbeProcess Identity;
}

public sealed class StartupProbeJobTerminationResult
{
    public bool Attempted;
    public bool Succeeded;
    public int ErrorCode;
}

public sealed class StartupProbeJobQueryAttempt
{
    public bool Attempted;
    public int AttemptNumber;
    public bool Succeeded;
    public int ErrorCode;
    public ulong CapacityBytes;
    public ulong RequiredBytes;
    public ulong ReturnLengthBytes;
    public uint AssignedProcessCount;
    public uint ListedProcessCount;
    public bool Resized;
}

public sealed class StartupProbeJobResult
{
    public IntPtr Handle;
    public bool Succeeded;
    public int ErrorCode;
    public int[] ProcessIds;
    public bool Attempted;
    public int AttemptCount;
    public ulong CapacityBytes;
    public ulong RequiredBytes;
    public ulong ReturnLengthBytes;
    public uint AssignedProcessCount;
    public uint ListedProcessCount;
    public bool Resized;
    public StartupProbeJobQueryAttempt[] Attempts;

    public StartupProbeJobResult()
    {
        ProcessIds = new int[0];
        Attempts = new StartupProbeJobQueryAttempt[0];
    }
}

public sealed class StartupProbeSuspendedProcessResult
{
    public IntPtr ProcessHandle;
    public IntPtr ThreadHandle;
    public int ProcessId;
    public bool Succeeded;
    public int ErrorCode;
}

public sealed class StartupProbeIdentityActionResult
{
    public bool Succeeded;
    public bool Existed;
    public int ErrorCode;
}

public sealed class StartupProbeProcessExitResult
{
    public bool Succeeded;
    public bool Active;
    public uint ExitCode;
    public int ErrorCode;
}

public static class NativeStartupProbe
{
    private const uint WM_CLOSE = 0x0010;
    private const uint TH32CS_SNAPPROCESS = 0x00000002;
    private const uint PROCESS_QUERY_LIMITED_INFORMATION = 0x1000;
    private const uint PROCESS_QUERY_INFORMATION = 0x0400;
    private const uint PROCESS_SET_INFORMATION = 0x0200;
    private const uint PROCESS_TERMINATE = 0x0001;
    private const uint PROCESS_SET_QUOTA = 0x0100;
    private const uint JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x00002000;
    private const uint CREATE_SUSPENDED = 0x00000004;
    private const uint CREATE_UNICODE_ENVIRONMENT = 0x00000400;
    private const int JobObjectBasicProcessIdList = 3;
    private const int JobObjectExtendedLimitInformation = 9;
    private const int ERROR_INSUFFICIENT_BUFFER = 122;
    private const int ERROR_NO_MORE_FILES = 18;
    private const int ERROR_BAD_LENGTH = 24;
    private const int ERROR_INVALID_DATA = 13;
    private const int ERROR_MORE_DATA = 234;
    private const int ERROR_INVALID_PARAMETER = 87;
    private const int ERROR_NOT_FOUND = 1168;
    private const int MAX_PROCESS_ENUMERATION_ATTEMPTS = 3;
    private const int MAX_PROCESS_ENTRY_COUNT = 65536;
    private const int MAX_JOB_QUERY_ATTEMPT_RECORDS = 8;
    private const uint MAX_JOB_QUERY_BYTES = 1024 * 1024;
    private const uint JOB_QUERY_HEADER_BYTES = 8;
    private const uint STILL_ACTIVE = 259;
    private const uint SIF_RANGE = 0x0001;
    private const int SB_CTL = 2;
    private const int GWL_STYLE = -16;
    private const long SBS_VERT = 0x0001;
    private static readonly IntPtr INVALID_HANDLE_VALUE = new IntPtr(-1);

    [StructLayout(LayoutKind.Sequential)]
    private struct RECT { public int Left; public int Top; public int Right; public int Bottom; }

    [StructLayout(LayoutKind.Sequential)]
    private struct FILETIME { public uint Low; public uint High; }

    [StructLayout(LayoutKind.Sequential)]
    private struct SCROLLINFO
    {
        public uint Size;
        public uint Mask;
        public int Minimum;
        public int Maximum;
        public uint Page;
        public int Position;
        public int TrackPosition;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct PROCESSENTRY32
    {
        public uint Size;
        public uint Usage;
        public uint ProcessId;
        public UIntPtr DefaultHeapId;
        public uint ModuleId;
        public uint Threads;
        public uint ParentProcessId;
        public int BasePriority;
        public uint Flags;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)]
        public string ExeFile;
    }

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
        public int dwX;
        public int dwY;
        public int dwXSize;
        public int dwYSize;
        public int dwXCountChars;
        public int dwYCountChars;
        public int dwFillAttribute;
        public int dwFlags;
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
        public uint dwProcessId;
        public uint dwThreadId;
    }

    private delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);
    private delegate IntPtr ProcessSnapshotInvoker(uint flags, uint processId, out int errorCode);
    private delegate bool ProcessEntryInvoker(IntPtr snapshot, ref PROCESSENTRY32 entry, out int errorCode);
    private delegate bool ProcessSnapshotCloseInvoker(IntPtr snapshot, out int errorCode);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool EnumWindows(EnumWindowsProc callback, IntPtr lParam);
    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool EnumChildWindows(IntPtr parent, EnumWindowsProc callback, IntPtr lParam);
    [DllImport("user32.dll", SetLastError = true)]
    private static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);
    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetWindowText(IntPtr hWnd, StringBuilder text, int maxCount);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetClassName(IntPtr hWnd, StringBuilder className, int maxCount);
    [DllImport("user32.dll", EntryPoint = "GetWindowLongPtrW", SetLastError = true)]
    private static extern IntPtr GetWindowLongPtr(IntPtr hWnd, int index);
    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool GetScrollInfo(IntPtr hWnd, int bar, ref SCROLLINFO info);
    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool PostMessage(IntPtr hWnd, uint message, IntPtr wParam, IntPtr lParam);
    [DllImport("dwmapi.dll", PreserveSig = true)]
    public static extern int DwmFlush();
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr CreateToolhelp32Snapshot(uint flags, uint processId);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool Process32FirstW(IntPtr snapshot, ref PROCESSENTRY32 entry);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool Process32NextW(IntPtr snapshot, ref PROCESSENTRY32 entry);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr OpenProcess(uint access, bool inheritHandle, uint processId);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true, EntryPoint = "CreateJobObjectW")]
    private static extern IntPtr CreateJobObject(IntPtr attributes, string name);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool SetInformationJobObject(IntPtr job, int informationClass, IntPtr information, uint informationLength);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool TerminateJobObject(IntPtr job, uint exitCode);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool QueryInformationJobObject(IntPtr job, int informationClass, IntPtr information, uint informationLength, out uint returnLength);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true, EntryPoint = "CreateProcessW")]
    private static extern bool CreateProcess(string applicationName, [In, Out] StringBuilder commandLine, IntPtr processAttributes, IntPtr threadAttributes, bool inheritHandles, uint creationFlags, IntPtr environment, string currentDirectory, ref STARTUPINFO startupInfo, out PROCESS_INFORMATION processInformation);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern uint ResumeThread(IntPtr thread);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool TerminateProcess(IntPtr process, uint exitCode);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GetProcessAffinityMask(IntPtr process, out UIntPtr processAffinityMask, out UIntPtr systemAffinityMask);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool SetProcessAffinityMask(IntPtr process, UIntPtr processAffinityMask);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool QueryFullProcessImageNameW(IntPtr process, uint flags, StringBuilder path, ref int size);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GetProcessTimes(IntPtr process, out FILETIME creation, out FILETIME exit, out FILETIME kernel, out FILETIME user);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GetExitCodeProcess(IntPtr process, out uint exitCode);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool CloseHandle(IntPtr handle);

    public static StartupProbeWindow[] GetTopLevelWindows()
    {
        var windows = new ArrayList();
        EnumWindows(delegate(IntPtr handle, IntPtr ignored) {
            uint pid;
            GetWindowThreadProcessId(handle, out pid);
            var title = new StringBuilder(1024);
            GetWindowText(handle, title, title.Capacity);
            var className = new StringBuilder(256);
            GetClassName(handle, className, className.Capacity);
            windows.Add(new StartupProbeWindow {
                Handle = handle,
                ProcessId = unchecked((int)pid),
                Visible = IsWindowVisible(handle),
                Caption = title.ToString(),
                ClassName = className.ToString()
            });
            return true;
        }, IntPtr.Zero);
        return (StartupProbeWindow[])windows.ToArray(typeof(StartupProbeWindow));
    }

    public static int GetVerticalScrollMaximum(IntPtr parent)
    {
        int maximum = -1;
        EnumChildWindows(parent, delegate(IntPtr handle, IntPtr ignored) {
            var className = new StringBuilder(256);
            GetClassName(handle, className, className.Capacity);
            if (!className.ToString().Equals("ScrollBar", StringComparison.OrdinalIgnoreCase)) return true;
            long style = GetWindowLongPtr(handle, GWL_STYLE).ToInt64();
            if ((style & SBS_VERT) == 0) return true;
            var info = new SCROLLINFO();
            info.Size = unchecked((uint)Marshal.SizeOf(typeof(SCROLLINFO)));
            info.Mask = SIF_RANGE;
            if (GetScrollInfo(handle, SB_CTL, ref info)) maximum = Math.Max(maximum, info.Maximum);
            return true;
        }, IntPtr.Zero);
        return maximum;
    }

    private static IntPtr InvokeProcessSnapshot(uint flags, uint processId, out int errorCode)
    {
        IntPtr snapshot = CreateToolhelp32Snapshot(flags, processId);
        errorCode = (snapshot == INVALID_HANDLE_VALUE || snapshot == IntPtr.Zero)
            ? Marshal.GetLastWin32Error() : 0;
        return snapshot;
    }

    private static bool InvokeProcess32First(IntPtr snapshot, ref PROCESSENTRY32 entry, out int errorCode)
    {
        bool succeeded = Process32FirstW(snapshot, ref entry);
        errorCode = succeeded ? 0 : Marshal.GetLastWin32Error();
        return succeeded;
    }

    private static bool InvokeProcess32Next(IntPtr snapshot, ref PROCESSENTRY32 entry, out int errorCode)
    {
        bool succeeded = Process32NextW(snapshot, ref entry);
        errorCode = succeeded ? 0 : Marshal.GetLastWin32Error();
        return succeeded;
    }

    private static bool InvokeProcessSnapshotClose(IntPtr snapshot, out int errorCode)
    {
        bool succeeded = CloseHandle(snapshot);
        errorCode = succeeded ? 0 : Marshal.GetLastWin32Error();
        return succeeded;
    }

    public static StartupProbeProcessEntriesResult GetProcessEntries()
    {
        return GetProcessEntriesCore(
            new ProcessSnapshotInvoker(InvokeProcessSnapshot),
            new ProcessEntryInvoker(InvokeProcess32First),
            new ProcessEntryInvoker(InvokeProcess32Next),
            new ProcessSnapshotCloseInvoker(InvokeProcessSnapshotClose));
    }

    private static int NormalizeProcessEnumerationError(int errorCode)
    {
        return errorCode == 0 ? ERROR_INVALID_DATA : errorCode;
    }

    private static bool TryAppendProcessEntry(PROCESSENTRY32 nativeEntry,
        ArrayList entries, Hashtable idsSeen, out int errorCode)
    {
        errorCode = 0;
        uint expectedSize = unchecked((uint)Marshal.SizeOf(typeof(PROCESSENTRY32)));
        if (nativeEntry.Size != expectedSize) {
            errorCode = ERROR_INVALID_DATA;
            return false;
        }
        // PID 0 is the real Windows System Idle Process and is therefore a
        // valid Toolhelp entry, even though it cannot be opened for identity.
        if (nativeEntry.ProcessId > Int32.MaxValue) {
            errorCode = ERROR_INVALID_DATA;
            return false;
        }
        if (nativeEntry.ParentProcessId > Int32.MaxValue) {
            errorCode = ERROR_INVALID_DATA;
            return false;
        }
        if (String.IsNullOrWhiteSpace(nativeEntry.ExeFile) || nativeEntry.ExeFile.Length >= 260) {
            errorCode = ERROR_INVALID_DATA;
            return false;
        }
        if (entries.Count >= MAX_PROCESS_ENTRY_COUNT) {
            errorCode = ERROR_INSUFFICIENT_BUFFER;
            return false;
        }
        int processId = unchecked((int)nativeEntry.ProcessId);
        if (idsSeen.ContainsKey(processId)) {
            errorCode = ERROR_INVALID_DATA;
            return false;
        }
        idsSeen.Add(processId, null);
        entries.Add(new StartupProbeProcessEntry {
            ProcessId = processId,
            ParentProcessId = unchecked((int)nativeEntry.ParentProcessId),
            ImageName = nativeEntry.ExeFile
        });
        return true;
    }

    private static StartupProbeProcessEntriesResult GetProcessEntriesCore(
        ProcessSnapshotInvoker snapshotInvoker, ProcessEntryInvoker firstInvoker,
        ProcessEntryInvoker nextInvoker, ProcessSnapshotCloseInvoker closeInvoker)
    {
        var result = new StartupProbeProcessEntriesResult();
        try {
            if (snapshotInvoker == null || firstInvoker == null || nextInvoker == null || closeInvoker == null) {
                result.ErrorCode = ERROR_INVALID_PARAMETER;
                return result;
            }
            while (result.AttemptCount < MAX_PROCESS_ENUMERATION_ATTEMPTS) {
                result.Attempted = true;
                result.AttemptCount++;
                bool retry = false;
                int failureCode = ERROR_INVALID_DATA;
                bool attemptComplete = false;
                StartupProbeProcessEntry[] completedEntries = null;
                IntPtr snapshot = INVALID_HANDLE_VALUE;
                var entries = new ArrayList();
                var idsSeen = new Hashtable();
                try {
                    int snapshotError = 0;
                    snapshot = snapshotInvoker(TH32CS_SNAPPROCESS, 0, out snapshotError);
                    if (snapshot == INVALID_HANDLE_VALUE || snapshot == IntPtr.Zero) {
                        failureCode = NormalizeProcessEnumerationError(snapshotError);
                        retry = failureCode == ERROR_BAD_LENGTH;
                    } else {
                        var nativeEntry = new PROCESSENTRY32();
                        nativeEntry.Size = unchecked((uint)Marshal.SizeOf(typeof(PROCESSENTRY32)));
                        int firstError = 0;
                        bool firstSucceeded = firstInvoker(snapshot, ref nativeEntry, out firstError);
                        if (!firstSucceeded) {
                            failureCode = NormalizeProcessEnumerationError(firstError);
                            if (failureCode == ERROR_NO_MORE_FILES) {
                                attemptComplete = true;
                                completedEntries = (StartupProbeProcessEntry[])entries.ToArray(typeof(StartupProbeProcessEntry));
                            }
                            else retry = failureCode == ERROR_BAD_LENGTH;
                        } else {
                            bool normalEnd = false;
                            while (true) {
                                int malformedError = 0;
                                if (!TryAppendProcessEntry(nativeEntry, entries, idsSeen, out malformedError)) {
                                    failureCode = malformedError;
                                    break;
                                }
                                int nextError = 0;
                                bool nextSucceeded = nextInvoker(snapshot, ref nativeEntry, out nextError);
                                if (nextSucceeded) continue;
                                failureCode = NormalizeProcessEnumerationError(nextError);
                                if (failureCode == ERROR_NO_MORE_FILES) {
                                    normalEnd = true;
                                    break;
                                }
                                retry = failureCode == ERROR_BAD_LENGTH;
                                break;
                            }
                            if (normalEnd) {
                                attemptComplete = true;
                                completedEntries = (StartupProbeProcessEntry[])entries.ToArray(typeof(StartupProbeProcessEntry));
                            }
                        }
                    }
                }
                catch (OutOfMemoryException) {
                    failureCode = 8;
                    retry = false;
                }
                catch (OverflowException) {
                    failureCode = ERROR_INVALID_DATA;
                    retry = false;
                }
                catch (Exception) {
                    failureCode = ERROR_INVALID_DATA;
                    retry = false;
                }
                finally {
                    if (snapshot != INVALID_HANDLE_VALUE && snapshot != IntPtr.Zero) {
                        try {
                            int closeError;
                            bool closeSucceeded = closeInvoker(snapshot, out closeError);
                            if (!closeSucceeded) {
                                if (attemptComplete) {
                                    failureCode = NormalizeProcessEnumerationError(closeError);
                                }
                                attemptComplete = false;
                                retry = false;
                            }
                        }
                        catch {
                            if (attemptComplete) failureCode = ERROR_INVALID_DATA;
                            attemptComplete = false;
                            retry = false;
                        }
                    }
                }
                if (attemptComplete) {
                    result.ErrorCode = 0;
                    result.Complete = true;
                    result.Succeeded = true;
                    result.Entries = completedEntries ?? new StartupProbeProcessEntry[0];
                    return result;
                }
                result.ErrorCode = failureCode;
                if (!retry || result.AttemptCount >= MAX_PROCESS_ENUMERATION_ATTEMPTS) return result;
                result.RetryCount++;
                result.Retried = true;
            }
            if (result.ErrorCode == 0) result.ErrorCode = ERROR_BAD_LENGTH;
        }
        catch (OutOfMemoryException) { result.ErrorCode = 8; }
        catch (OverflowException) { result.ErrorCode = ERROR_INVALID_DATA; }
        catch (Exception) { result.ErrorCode = ERROR_INVALID_DATA; }
        return result;
    }

    private static void SetSyntheticProcessEntry(ref PROCESSENTRY32 entry,
        uint processId, uint parentProcessId, string imageName)
    {
        entry.Size = unchecked((uint)Marshal.SizeOf(typeof(PROCESSENTRY32)));
        entry.ProcessId = processId;
        entry.ParentProcessId = parentProcessId;
        entry.ExeFile = imageName;
    }

    public static bool RunProcessEnumerationContractSelfTest()
    {
        // A false First call with ERROR_NO_MORE_FILES is the only valid empty
        // enumeration.  The same injected core drives this and every case
        // below, so the tests exercise the production retry state machine.
        int emptySnapshotCalls = 0;
        int emptyFirstCalls = 0;
        int emptyNextCalls = 0;
        int emptyCloseCalls = 0;
        ProcessSnapshotInvoker emptySnapshot = delegate(uint flags, uint processId, out int errorCode) {
            emptySnapshotCalls++;
            errorCode = 0;
            return new IntPtr(emptySnapshotCalls);
        };
        ProcessEntryInvoker emptyFirst = delegate(IntPtr snapshot, ref PROCESSENTRY32 entry, out int errorCode) {
            emptyFirstCalls++;
            errorCode = ERROR_NO_MORE_FILES;
            return false;
        };
        ProcessEntryInvoker emptyNext = delegate(IntPtr snapshot, ref PROCESSENTRY32 entry, out int errorCode) {
            emptyNextCalls++;
            errorCode = ERROR_NO_MORE_FILES;
            return false;
        };
        ProcessSnapshotCloseInvoker emptyClose = delegate(IntPtr snapshot, out int errorCode) {
            emptyCloseCalls++;
            errorCode = 0;
            return true;
        };
        StartupProbeProcessEntriesResult emptyResult = GetProcessEntriesCore(
            emptySnapshot, emptyFirst, emptyNext, emptyClose);
        if (!emptyResult.Complete || !emptyResult.Succeeded || emptyResult.ErrorCode != 0 ||
            emptyResult.AttemptCount != 1 || emptyResult.RetryCount != 0 || emptyResult.Retried ||
            emptyResult.Entries == null || emptyResult.Entries.Length != 0 ||
            emptySnapshotCalls != 1 || emptyFirstCalls != 1 || emptyNextCalls != 0 || emptyCloseCalls != 1) {
            return false;
        }

        // A valid snapshot whose close operation fails cannot publish even an
        // otherwise complete empty enumeration.
        int closeFailureSnapshotCalls = 0;
        int closeFailureFirstCalls = 0;
        int closeFailureNextCalls = 0;
        int closeFailureCloseCalls = 0;
        ProcessSnapshotInvoker closeFailureSnapshot = delegate(uint flags, uint processId, out int errorCode) {
            closeFailureSnapshotCalls++;
            errorCode = 0;
            return new IntPtr(1);
        };
        ProcessEntryInvoker closeFailureFirst = delegate(IntPtr snapshot, ref PROCESSENTRY32 entry, out int errorCode) {
            closeFailureFirstCalls++;
            errorCode = ERROR_NO_MORE_FILES;
            return false;
        };
        ProcessEntryInvoker closeFailureNext = delegate(IntPtr snapshot, ref PROCESSENTRY32 entry, out int errorCode) {
            closeFailureNextCalls++;
            errorCode = ERROR_NO_MORE_FILES;
            return false;
        };
        ProcessSnapshotCloseInvoker closeFailureClose = delegate(IntPtr snapshot, out int errorCode) {
            closeFailureCloseCalls++;
            errorCode = 5;
            return false;
        };
        StartupProbeProcessEntriesResult closeFailureResult = GetProcessEntriesCore(
            closeFailureSnapshot, closeFailureFirst, closeFailureNext, closeFailureClose);
        if (closeFailureResult.Complete || closeFailureResult.Succeeded || closeFailureResult.ErrorCode != 5 ||
            closeFailureResult.AttemptCount != 1 || closeFailureResult.RetryCount != 0 || closeFailureResult.Retried ||
            closeFailureResult.Entries == null || closeFailureResult.Entries.Length != 0 ||
            closeFailureSnapshotCalls != 1 || closeFailureFirstCalls != 1 || closeFailureNextCalls != 0 ||
            closeFailureCloseCalls != 1) {
            return false;
        }

        // A one-entry list is complete only after Next reports the normal
        // ERROR_NO_MORE_FILES terminator.
        int oneEntrySnapshotCalls = 0;
        int oneEntryFirstCalls = 0;
        int oneEntryNextCalls = 0;
        ProcessSnapshotInvoker oneEntrySnapshot = delegate(uint flags, uint processId, out int errorCode) {
            oneEntrySnapshotCalls++;
            errorCode = 0;
            return new IntPtr(oneEntrySnapshotCalls);
        };
        ProcessEntryInvoker oneEntryFirst = delegate(IntPtr snapshot, ref PROCESSENTRY32 entry, out int errorCode) {
            oneEntryFirstCalls++;
            SetSyntheticProcessEntry(ref entry, 101, 1, "one-entry.exe");
            errorCode = 0;
            return true;
        };
        ProcessEntryInvoker oneEntryNext = delegate(IntPtr snapshot, ref PROCESSENTRY32 entry, out int errorCode) {
            oneEntryNextCalls++;
            errorCode = ERROR_NO_MORE_FILES;
            return false;
        };
        ProcessSnapshotCloseInvoker oneEntryClose = delegate(IntPtr snapshot, out int errorCode) {
            errorCode = 0;
            return true;
        };
        StartupProbeProcessEntriesResult oneEntryResult = GetProcessEntriesCore(
            oneEntrySnapshot, oneEntryFirst, oneEntryNext, oneEntryClose);
        if (!oneEntryResult.Complete || !oneEntryResult.Succeeded || oneEntryResult.ErrorCode != 0 ||
            oneEntryResult.AttemptCount != 1 || oneEntryResult.RetryCount != 0 || oneEntryResult.Retried ||
            oneEntryResult.Entries == null || oneEntryResult.Entries.Length != 1 ||
            oneEntryResult.Entries[0].ProcessId != 101 || oneEntryResult.Entries[0].ParentProcessId != 1 ||
            !String.Equals(oneEntryResult.Entries[0].ImageName, "one-entry.exe", StringComparison.Ordinal) ||
            oneEntrySnapshotCalls != 1 || oneEntryFirstCalls != 1 || oneEntryNextCalls != 1) {
            return false;
        }

        // ERROR_BAD_LENGTH on First restarts from a new snapshot, and the
        // successful second attempt is the only attempt whose entries publish.
        int retrySnapshotCalls = 0;
        int retryFirstCalls = 0;
        int retryNextCalls = 0;
        ProcessSnapshotInvoker retrySnapshot = delegate(uint flags, uint processId, out int errorCode) {
            retrySnapshotCalls++;
            errorCode = 0;
            return new IntPtr(retrySnapshotCalls);
        };
        ProcessEntryInvoker retryFirst = delegate(IntPtr snapshot, ref PROCESSENTRY32 entry, out int errorCode) {
            retryFirstCalls++;
            if (snapshot.ToInt64() == 1) {
                errorCode = ERROR_BAD_LENGTH;
                return false;
            }
            SetSyntheticProcessEntry(ref entry, 202, 1, "retry-success.exe");
            errorCode = 0;
            return true;
        };
        ProcessEntryInvoker retryNext = delegate(IntPtr snapshot, ref PROCESSENTRY32 entry, out int errorCode) {
            retryNextCalls++;
            errorCode = ERROR_NO_MORE_FILES;
            return false;
        };
        ProcessSnapshotCloseInvoker retryClose = delegate(IntPtr snapshot, out int errorCode) {
            errorCode = 0;
            return true;
        };
        StartupProbeProcessEntriesResult retryResult = GetProcessEntriesCore(
            retrySnapshot, retryFirst, retryNext, retryClose);
        if (!retryResult.Complete || !retryResult.Succeeded || retryResult.ErrorCode != 0 ||
            retryResult.AttemptCount != 2 || retryResult.RetryCount != 1 || !retryResult.Retried ||
            retryResult.Entries == null || retryResult.Entries.Length != 1 ||
            retryResult.Entries[0].ProcessId != 202 || retrySnapshotCalls != 2 ||
            retryFirstCalls != 2 || retryNextCalls != 1) {
            return false;
        }

        // ERROR_BAD_LENGTH on Next after one observed entry must restart from
        // a fresh snapshot and discard that first attempt's entry.
        int nextRetrySnapshotCalls = 0;
        int nextRetryFirstCalls = 0;
        int nextRetryNextCalls = 0;
        int nextRetryCloseCalls = 0;
        ProcessSnapshotInvoker nextRetrySnapshot = delegate(uint flags, uint processId, out int errorCode) {
            nextRetrySnapshotCalls++;
            errorCode = 0;
            return new IntPtr(nextRetrySnapshotCalls);
        };
        ProcessEntryInvoker nextRetryFirst = delegate(IntPtr snapshot, ref PROCESSENTRY32 entry, out int errorCode) {
            nextRetryFirstCalls++;
            if (snapshot.ToInt64() == 1) {
                SetSyntheticProcessEntry(ref entry, 212, 1, "next-retry-discarded.exe");
            } else {
                SetSyntheticProcessEntry(ref entry, 213, 1, "next-retry-success.exe");
            }
            errorCode = 0;
            return true;
        };
        ProcessEntryInvoker nextRetryNext = delegate(IntPtr snapshot, ref PROCESSENTRY32 entry, out int errorCode) {
            nextRetryNextCalls++;
            errorCode = snapshot.ToInt64() == 1 ? ERROR_BAD_LENGTH : ERROR_NO_MORE_FILES;
            return false;
        };
        ProcessSnapshotCloseInvoker nextRetryClose = delegate(IntPtr snapshot, out int errorCode) {
            nextRetryCloseCalls++;
            errorCode = 0;
            return true;
        };
        StartupProbeProcessEntriesResult nextRetryResult = GetProcessEntriesCore(
            nextRetrySnapshot, nextRetryFirst, nextRetryNext, nextRetryClose);
        if (!nextRetryResult.Complete || !nextRetryResult.Succeeded || nextRetryResult.ErrorCode != 0 ||
            nextRetryResult.AttemptCount != 2 || nextRetryResult.RetryCount != 1 || !nextRetryResult.Retried ||
            nextRetryResult.Entries == null || nextRetryResult.Entries.Length != 1 ||
            nextRetryResult.Entries[0].ProcessId != 213 ||
            !String.Equals(nextRetryResult.Entries[0].ImageName, "next-retry-success.exe", StringComparison.Ordinal) ||
            nextRetrySnapshotCalls != 2 || nextRetryFirstCalls != 2 || nextRetryNextCalls != 2 ||
            nextRetryCloseCalls != 2) {
            return false;
        }

        // Entries observed before a non-retryable Next failure are never
        // published as a partial success.
        int partialSnapshotCalls = 0;
        int partialFirstCalls = 0;
        int partialNextCalls = 0;
        ProcessSnapshotInvoker partialSnapshot = delegate(uint flags, uint processId, out int errorCode) {
            partialSnapshotCalls++;
            errorCode = 0;
            return new IntPtr(partialSnapshotCalls);
        };
        ProcessEntryInvoker partialFirst = delegate(IntPtr snapshot, ref PROCESSENTRY32 entry, out int errorCode) {
            partialFirstCalls++;
            SetSyntheticProcessEntry(ref entry, 303, 1, "partial-failure.exe");
            errorCode = 0;
            return true;
        };
        ProcessEntryInvoker partialNext = delegate(IntPtr snapshot, ref PROCESSENTRY32 entry, out int errorCode) {
            partialNextCalls++;
            errorCode = 5;
            return false;
        };
        ProcessSnapshotCloseInvoker partialClose = delegate(IntPtr snapshot, out int errorCode) {
            errorCode = 0;
            return true;
        };
        StartupProbeProcessEntriesResult partialResult = GetProcessEntriesCore(
            partialSnapshot, partialFirst, partialNext, partialClose);
        if (partialResult.Complete || partialResult.Succeeded || partialResult.ErrorCode != 5 ||
            partialResult.AttemptCount != 1 || partialResult.Entries == null ||
            partialResult.Entries.Length != 0 || partialSnapshotCalls != 1 ||
            partialFirstCalls != 1 || partialNextCalls != 1) {
            return false;
        }

        // Three retryable failures consume the complete bounded budget.  There
        // must be no fourth snapshot or First call after the third ERROR_BAD_LENGTH.
        int exhaustedSnapshotCalls = 0;
        int exhaustedFirstCalls = 0;
        int exhaustedNextCalls = 0;
        ProcessSnapshotInvoker exhaustedSnapshot = delegate(uint flags, uint processId, out int errorCode) {
            exhaustedSnapshotCalls++;
            errorCode = 0;
            return new IntPtr(exhaustedSnapshotCalls);
        };
        ProcessEntryInvoker exhaustedFirst = delegate(IntPtr snapshot, ref PROCESSENTRY32 entry, out int errorCode) {
            exhaustedFirstCalls++;
            errorCode = ERROR_BAD_LENGTH;
            return false;
        };
        ProcessEntryInvoker exhaustedNext = delegate(IntPtr snapshot, ref PROCESSENTRY32 entry, out int errorCode) {
            exhaustedNextCalls++;
            errorCode = ERROR_NO_MORE_FILES;
            return false;
        };
        ProcessSnapshotCloseInvoker exhaustedClose = delegate(IntPtr snapshot, out int errorCode) {
            errorCode = 0;
            return true;
        };
        StartupProbeProcessEntriesResult exhaustedResult = GetProcessEntriesCore(
            exhaustedSnapshot, exhaustedFirst, exhaustedNext, exhaustedClose);
        if (exhaustedResult.Complete || exhaustedResult.Succeeded ||
            exhaustedResult.ErrorCode != ERROR_BAD_LENGTH ||
            exhaustedResult.AttemptCount != MAX_PROCESS_ENUMERATION_ATTEMPTS ||
            exhaustedResult.RetryCount != MAX_PROCESS_ENUMERATION_ATTEMPTS - 1 ||
            !exhaustedResult.Retried || exhaustedResult.Entries == null ||
            exhaustedResult.Entries.Length != 0 || exhaustedSnapshotCalls != MAX_PROCESS_ENUMERATION_ATTEMPTS ||
            exhaustedFirstCalls != MAX_PROCESS_ENUMERATION_ATTEMPTS || exhaustedNextCalls != 0) {
            return false;
        }
        return true;
    }

    public static StartupProbeProcess GetProcessIdentity(int processId, int parentProcessId)
    {
        IntPtr process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, unchecked((uint)processId));
        if (process == IntPtr.Zero) return null;
        try {
            var path = new StringBuilder(32768);
            int pathLength = path.Capacity;
            FILETIME creation, exit, kernel, user;
            if (!QueryFullProcessImageNameW(process, 0, path, ref pathLength) ||
                !GetProcessTimes(process, out creation, out exit, out kernel, out user)) return null;
            long creationTime = unchecked((long)(((ulong)creation.High << 32) | creation.Low));
            return new StartupProbeProcess {
                ProcessId = processId,
                ParentProcessId = parentProcessId,
                CreationTime = creationTime,
                ImagePath = path.ToString()
            };
        }
        finally { CloseHandle(process); }
    }

    public static StartupProbeProcessIdentityResult QueryProcessIdentity(int processId, int parentProcessId)
    {
        var result = new StartupProbeProcessIdentityResult { Succeeded = false, ErrorCode = 0, Operation = "open-process", Identity = null };
        IntPtr process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, unchecked((uint)processId));
        if (process == IntPtr.Zero) {
            result.ErrorCode = Marshal.GetLastWin32Error();
            return result;
        }
        try {
            var path = new StringBuilder(32768);
            int pathLength = path.Capacity;
            FILETIME creation, exit, kernel, user;
            result.Operation = "query-image-path";
            if (!QueryFullProcessImageNameW(process, 0, path, ref pathLength)) {
                result.ErrorCode = Marshal.GetLastWin32Error();
                return result;
            }
            result.Operation = "get-process-times";
            if (!GetProcessTimes(process, out creation, out exit, out kernel, out user)) {
                result.ErrorCode = Marshal.GetLastWin32Error();
                return result;
            }
            long creationTime = unchecked((long)(((ulong)creation.High << 32) | creation.Low));
            result.Identity = new StartupProbeProcess {
                ProcessId = processId,
                ParentProcessId = parentProcessId,
                CreationTime = creationTime,
                ImagePath = path.ToString()
            };
            result.Succeeded = true;
            result.Operation = "none";
            return result;
        }
        finally { CloseHandle(process); }
    }

    public static StartupProbeProcessExitResult QueryProcessExitState(IntPtr process)
    {
        var result = new StartupProbeProcessExitResult {
            Succeeded = false,
            Active = false,
            ExitCode = 0,
            ErrorCode = ERROR_INVALID_PARAMETER
        };
        if (process == IntPtr.Zero) return result;
        uint exitCode;
        if (!GetExitCodeProcess(process, out exitCode)) {
            result.ErrorCode = Marshal.GetLastWin32Error();
            return result;
        }
        result.Succeeded = true;
        result.ExitCode = exitCode;
        result.Active = exitCode == STILL_ACTIVE;
        result.ErrorCode = 0;
        return result;
    }

    public static StartupProbeSuspendedProcessResult CreateSuspendedProcess(string applicationPath, string commandLine, string workingDirectory, string[] environmentOverrides)
    {
        var result = new StartupProbeSuspendedProcessResult {
            ProcessHandle = IntPtr.Zero,
            ThreadHandle = IntPtr.Zero,
            ProcessId = 0,
            Succeeded = false,
            ErrorCode = 0
        };
        if (String.IsNullOrWhiteSpace(applicationPath) || String.IsNullOrWhiteSpace(commandLine) || String.IsNullOrWhiteSpace(workingDirectory)) {
            result.ErrorCode = ERROR_INVALID_PARAMETER;
            return result;
        }

        var mergedEnvironment = new Hashtable(StringComparer.OrdinalIgnoreCase);
        foreach (DictionaryEntry entry in Environment.GetEnvironmentVariables()) {
            if (entry.Key != null) mergedEnvironment[(string)entry.Key] = entry.Value == null ? String.Empty : (string)entry.Value;
        }
        if (environmentOverrides != null) {
            foreach (string value in environmentOverrides) {
                if (String.IsNullOrEmpty(value)) { result.ErrorCode = ERROR_INVALID_PARAMETER; return result; }
                int separator = value.IndexOf('=');
                if (separator <= 0) { result.ErrorCode = ERROR_INVALID_PARAMETER; return result; }
                string key = value.Substring(0, separator);
                if (key.IndexOf('\0') >= 0 || key.IndexOf('=') >= 0) { result.ErrorCode = ERROR_INVALID_PARAMETER; return result; }
                mergedEnvironment[key] = value.Substring(separator + 1);
            }
        }
        var environmentKeys = new ArrayList();
        foreach (string key in mergedEnvironment.Keys) environmentKeys.Add(key);
        environmentKeys.Sort(StringComparer.OrdinalIgnoreCase);
        var environmentBlock = new StringBuilder();
        foreach (string key in environmentKeys) {
            environmentBlock.Append(key).Append('=').Append(mergedEnvironment[key]).Append('\0');
        }
        environmentBlock.Append('\0');
        IntPtr nativeEnvironment = Marshal.StringToHGlobalUni(environmentBlock.ToString());
        try {
            var startupInfo = new STARTUPINFO();
            startupInfo.cb = Marshal.SizeOf(typeof(STARTUPINFO));
            var processInformation = new PROCESS_INFORMATION();
            var mutableCommandLine = new StringBuilder(commandLine);
            if (!CreateProcess(applicationPath, mutableCommandLine, IntPtr.Zero, IntPtr.Zero, false,
                CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT, nativeEnvironment, workingDirectory,
                ref startupInfo, out processInformation)) {
                result.ErrorCode = Marshal.GetLastWin32Error();
                return result;
            }
            result.ProcessHandle = processInformation.hProcess;
            result.ThreadHandle = processInformation.hThread;
            result.ProcessId = unchecked((int)processInformation.dwProcessId);
            result.Succeeded = result.ProcessHandle != IntPtr.Zero && result.ThreadHandle != IntPtr.Zero && result.ProcessId > 0;
            if (!result.Succeeded) {
                if (result.ProcessHandle != IntPtr.Zero) TerminateProcess(result.ProcessHandle, 1);
                if (result.ThreadHandle != IntPtr.Zero) CloseHandle(result.ThreadHandle);
                if (result.ProcessHandle != IntPtr.Zero) CloseHandle(result.ProcessHandle);
                result.ProcessHandle = IntPtr.Zero;
                result.ThreadHandle = IntPtr.Zero;
                result.ProcessId = 0;
                result.ErrorCode = ERROR_INVALID_PARAMETER;
            }
            return result;
        }
        finally { Marshal.FreeHGlobal(nativeEnvironment); }
    }

    public static StartupProbeIdentityActionResult ResumeSuspendedProcess(IntPtr thread)
    {
        var result = new StartupProbeIdentityActionResult { Succeeded = false, Existed = thread != IntPtr.Zero, ErrorCode = 0 };
        if (thread == IntPtr.Zero) { result.ErrorCode = ERROR_INVALID_PARAMETER; return result; }
        if (ResumeThread(thread) == UInt32.MaxValue) {
            result.ErrorCode = Marshal.GetLastWin32Error();
            return result;
        }
        result.Succeeded = true;
        return result;
    }

    public static StartupProbeIdentityActionResult TerminateProcessHandle(IntPtr process)
    {
        var result = new StartupProbeIdentityActionResult { Succeeded = false, Existed = process != IntPtr.Zero, ErrorCode = 0 };
        if (process == IntPtr.Zero) { result.ErrorCode = ERROR_INVALID_PARAMETER; return result; }
        if (!TerminateProcess(process, 1)) {
            result.ErrorCode = Marshal.GetLastWin32Error();
            return result;
        }
        result.Succeeded = true;
        return result;
    }

    public static StartupProbeIdentityActionResult TerminateProcessIdentity(int processId, long creationTime, string expectedImagePath)
    {
        var result = new StartupProbeIdentityActionResult { Succeeded = false, Existed = false, ErrorCode = 0 };
        if (processId <= 0 || String.IsNullOrWhiteSpace(expectedImagePath)) { result.ErrorCode = ERROR_INVALID_PARAMETER; return result; }
        IntPtr process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, false, unchecked((uint)processId));
        if (process == IntPtr.Zero) {
            result.ErrorCode = Marshal.GetLastWin32Error();
            result.Existed = result.ErrorCode != ERROR_INVALID_PARAMETER && result.ErrorCode != ERROR_NOT_FOUND;
            return result;
        }
        result.Existed = true;
        try {
            var path = new StringBuilder(32768);
            int pathLength = path.Capacity;
            FILETIME creation, exit, kernel, user;
            if (!QueryFullProcessImageNameW(process, 0, path, ref pathLength) ||
                !GetProcessTimes(process, out creation, out exit, out kernel, out user)) {
                result.ErrorCode = Marshal.GetLastWin32Error();
                return result;
            }
            long observedCreation = unchecked((long)(((ulong)creation.High << 32) | creation.Low));
            if (observedCreation != creationTime || !String.Equals(path.ToString(), expectedImagePath, StringComparison.OrdinalIgnoreCase)) {
                result.ErrorCode = ERROR_NOT_FOUND;
                return result;
            }
            if (!TerminateProcess(process, 1)) {
                result.ErrorCode = Marshal.GetLastWin32Error();
                return result;
            }
            result.Succeeded = true;
            return result;
        }
        finally { CloseHandle(process); }
    }

    public static bool CloseNativeHandle(IntPtr handle)
    {
        return handle == IntPtr.Zero || CloseHandle(handle);
    }

    public static StartupProbeJobResult CreateKillOnCloseJob()
    {
        var result = new StartupProbeJobResult { Handle = IntPtr.Zero, Succeeded = false, ErrorCode = 0, ProcessIds = new int[0] };
        IntPtr job = CreateJobObject(IntPtr.Zero, null);
        if (job == IntPtr.Zero) {
            result.ErrorCode = Marshal.GetLastWin32Error();
            return result;
        }
        var limits = new JOBOBJECT_EXTENDED_LIMIT_INFORMATION();
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        IntPtr nativeLimits = Marshal.AllocHGlobal(Marshal.SizeOf(typeof(JOBOBJECT_EXTENDED_LIMIT_INFORMATION)));
        try {
            Marshal.StructureToPtr(limits, nativeLimits, false);
            if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, nativeLimits, unchecked((uint)Marshal.SizeOf(typeof(JOBOBJECT_EXTENDED_LIMIT_INFORMATION))))) {
                result.ErrorCode = Marshal.GetLastWin32Error();
                CloseHandle(job);
                return result;
            }
        }
        finally { Marshal.FreeHGlobal(nativeLimits); }
        result.Handle = job;
        result.Succeeded = true;
        return result;
    }

    public static StartupProbeJobResult AssignProcessToKillOnCloseJob(IntPtr job, int processId)
    {
        var result = new StartupProbeJobResult { Handle = job, Succeeded = false, ErrorCode = 0, ProcessIds = new int[0] };
        if (job == IntPtr.Zero) { result.ErrorCode = 6; return result; }
        IntPtr process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_QUOTA | PROCESS_TERMINATE, false, unchecked((uint)processId));
        if (process == IntPtr.Zero) {
            result.ErrorCode = Marshal.GetLastWin32Error();
            return result;
        }
        try {
            if (!AssignProcessToJobObject(job, process)) {
                result.ErrorCode = Marshal.GetLastWin32Error();
                return result;
            }
            result.Succeeded = true;
            return result;
        }
        finally { CloseHandle(process); }
    }

    public static StartupProbeJobTerminationResult TerminateKillOnCloseJob(IntPtr job)
    {
        var result = new StartupProbeJobTerminationResult { Attempted = true, Succeeded = false, ErrorCode = 0 };
        if (job == IntPtr.Zero) { result.ErrorCode = 6; return result; }
        if (!TerminateJobObject(job, 1)) {
            result.ErrorCode = Marshal.GetLastWin32Error();
            return result;
        }
        result.Succeeded = true;
        return result;
    }

    private static uint BoundJobQueryByteCount(uint value)
    {
        return value > MAX_JOB_QUERY_BYTES ? MAX_JOB_QUERY_BYTES : value;
    }

    private static void RecordJobQueryAttempt(StartupProbeJobResult result, bool succeeded, int errorCode,
        uint capacityBytes, uint requiredBytes, uint returnLengthBytes, uint assignedProcessCount,
        uint listedProcessCount, bool resized)
    {
        result.Attempted = true;
        result.AttemptCount++;
        result.CapacityBytes = BoundJobQueryByteCount(capacityBytes);
        result.RequiredBytes = BoundJobQueryByteCount(requiredBytes);
        result.ReturnLengthBytes = BoundJobQueryByteCount(returnLengthBytes);
        result.AssignedProcessCount = assignedProcessCount;
        result.ListedProcessCount = listedProcessCount;
        result.Resized = result.Resized || resized;
        try {
            if (result.Attempts == null || result.Attempts.Length >= MAX_JOB_QUERY_ATTEMPT_RECORDS) return;
            var observations = new StartupProbeJobQueryAttempt[result.Attempts.Length + 1];
            Array.Copy(result.Attempts, observations, result.Attempts.Length);
            observations[observations.Length - 1] = new StartupProbeJobQueryAttempt {
                Attempted = true,
                AttemptNumber = result.AttemptCount,
                Succeeded = succeeded,
                ErrorCode = succeeded ? 0 : errorCode,
                CapacityBytes = BoundJobQueryByteCount(capacityBytes),
                RequiredBytes = BoundJobQueryByteCount(requiredBytes),
                ReturnLengthBytes = BoundJobQueryByteCount(returnLengthBytes),
                AssignedProcessCount = assignedProcessCount,
                ListedProcessCount = listedProcessCount,
                Resized = resized
            };
            result.Attempts = observations;
        }
        catch { }
    }

    private static bool IsRetryableJobQueryError(int errorCode)
    {
        return errorCode == ERROR_INSUFFICIENT_BUFFER || errorCode == ERROR_BAD_LENGTH || errorCode == ERROR_MORE_DATA;
    }

    private static bool TryGetLargerJobQueryCapacity(uint currentCapacity, uint returnedBytes,
        uint assignedProcessCount, out uint nextCapacity)
    {
        nextCapacity = 0;
        try {
            ulong candidate = returnedBytes;
            ulong byCount = checked(JOB_QUERY_HEADER_BYTES + checked((ulong)assignedProcessCount * (ulong)IntPtr.Size));
            if (byCount > candidate) candidate = byCount;

            // A retry must always make progress.  The kernel may return an
            // unchanged/zero length for ERROR_BAD_LENGTH or ERROR_MORE_DATA,
            // so use checked geometric growth when the returned hints do not
            // provide a larger buffer.
            if (candidate <= currentCapacity) {
                candidate = checked((ulong)currentCapacity * 2UL);
            }
            if (candidate <= currentCapacity || candidate > MAX_JOB_QUERY_BYTES || candidate > UInt32.MaxValue) return false;
            nextCapacity = (uint)candidate;
            return true;
        }
        catch (OverflowException) { return false; }
    }

    private static bool IsJobQueryCountsAndCapacityValid(uint capacityBytes,
        uint assignedProcessCount, uint listedProcessCount)
    {
        try {
            if (capacityBytes < JOB_QUERY_HEADER_BYTES || capacityBytes > MAX_JOB_QUERY_BYTES) return false;
            if (listedProcessCount > assignedProcessCount) return false;
            ulong capacitySlots = (capacityBytes - JOB_QUERY_HEADER_BYTES) / (ulong)IntPtr.Size;
            return (ulong)listedProcessCount <= capacitySlots;
        }
        catch (OverflowException) { return false; }
    }

    private static bool IsJobQueryReturnLengthValid(uint capacityBytes, uint returnLengthBytes,
        uint listedProcessCount)
    {
        try {
            if (capacityBytes < JOB_QUERY_HEADER_BYTES || capacityBytes > MAX_JOB_QUERY_BYTES) return false;
            ulong minimumReturn = checked(JOB_QUERY_HEADER_BYTES + checked((ulong)listedProcessCount * (ulong)IntPtr.Size));
            // Some Windows versions report zero bytes for an empty list even
            // though the two count fields were returned.  The caller separately
            // verifies that assigned is also zero for that exact empty shape.
            if (returnLengthBytes == 0) return listedProcessCount == 0;
            return returnLengthBytes >= minimumReturn && returnLengthBytes <= capacityBytes;
        }
        catch (OverflowException) { return false; }
    }

    private static bool IsJobQueryShapeStructurallyValid(uint capacityBytes, uint returnLengthBytes,
        uint assignedProcessCount, uint listedProcessCount)
    {
        if (!IsJobQueryCountsAndCapacityValid(capacityBytes, assignedProcessCount, listedProcessCount)) return false;
        if (returnLengthBytes == 0) return assignedProcessCount == 0 && listedProcessCount == 0;
        return IsJobQueryReturnLengthValid(capacityBytes, returnLengthBytes, listedProcessCount);
    }

    // This predicate is shared by the production enumeration and the
    // no-GUI contract self-test.  The sizing call consumes attempt zero, so
    // each enumeration may issue attempts 0 through 7 and never a ninth call.
    private static bool CanAttemptJobQuery(int attemptCount)
    {
        return attemptCount >= 0 && attemptCount < MAX_JOB_QUERY_ATTEMPT_RECORDS;
    }

    // Keep the bounded arithmetic, retry loop, and malformed-shape branches
    // executable in the no-GUI self-test without exposing them as a
    // measurement API.  The injected invokers exercise QueryJobProcessIdsCore
    // itself, including its shared attempt predicate and telemetry journal.
    public static bool RunJobQueryContractSelfTest()
    {
        uint next;
        if (!IsRetryableJobQueryError(ERROR_INSUFFICIENT_BUFFER) ||
            !IsRetryableJobQueryError(ERROR_BAD_LENGTH) ||
            !IsRetryableJobQueryError(ERROR_MORE_DATA) ||
            IsRetryableJobQueryError(ERROR_INVALID_PARAMETER)) return false;

        // Reproduce the observed ERROR_MORE_DATA correction: sizing reports
        // 16, the first data query reports 234 and requires 40, then a complete
        // list succeeds.  This is exactly three native calls: sizing, retry,
        // and final success.
        int moreDataCalls = 0;
        JobQueryInvoker moreDataInvoker = delegate(IntPtr job, IntPtr buffer, uint capacity,
            out uint returnLength, out int errorCode) {
            int call = moreDataCalls++;
            if (call == 0 && capacity == 0) {
                returnLength = 16;
                errorCode = ERROR_BAD_LENGTH;
                return false;
            }
            if (call == 1 && capacity == 16) {
                returnLength = 40;
                errorCode = ERROR_MORE_DATA;
                return false;
            }
            if (call == 2 && capacity == 40 && buffer != IntPtr.Zero) {
                WriteSyntheticJobList(buffer, capacity, 2, 2, 101, 102);
                returnLength = 24;
                errorCode = 0;
                return true;
            }
            returnLength = 0;
            errorCode = ERROR_INVALID_PARAMETER;
            return false;
        };
        StartupProbeJobResult moreDataResult = QueryJobProcessIdsCore(new IntPtr(1), moreDataInvoker);
        if (!moreDataResult.Succeeded || moreDataCalls != 3 ||
            moreDataResult.AttemptCount != 3 || moreDataResult.Attempts.Length != 3 ||
            moreDataResult.Attempts[0].CapacityBytes != 0 ||
            moreDataResult.Attempts[1].CapacityBytes != 16 ||
            moreDataResult.Attempts[1].ErrorCode != ERROR_MORE_DATA ||
            !moreDataResult.Attempts[1].Resized ||
            moreDataResult.Attempts[2].CapacityBytes != 40 ||
            moreDataResult.Attempts[2].ListedProcessCount != 2 ||
            moreDataResult.ProcessIds.Length != 2 ||
            moreDataResult.ProcessIds[0] != 101 || moreDataResult.ProcessIds[1] != 102) return false;

        // A successful partial list is not success: the same production loop
        // must grow and accept only the subsequent complete list.  This is
        // also exactly three native calls.  Derive both architecture-sensitive
        // capacities from the production growth helper.
        uint partialNextCapacity;
        if (!TryGetLargerJobQueryCapacity(16, 16, 2, out partialNextCapacity) ||
            partialNextCapacity <= 16) return false;
        uint partialFinalReturnLength = checked(JOB_QUERY_HEADER_BYTES + (2U * (uint)IntPtr.Size));
        int partialCalls = 0;
        JobQueryInvoker partialInvoker = delegate(IntPtr job, IntPtr buffer, uint capacity,
            out uint returnLength, out int errorCode) {
            int call = partialCalls++;
            if (call == 0 && capacity == 0) {
                returnLength = 16;
                errorCode = ERROR_INSUFFICIENT_BUFFER;
                return false;
            }
            if (call == 1 && capacity == 16 && buffer != IntPtr.Zero) {
                WriteSyntheticJobList(buffer, capacity, 2, 1, 201, 0);
                returnLength = 16;
                errorCode = 0;
                return true;
            }
            if (call == 2 && capacity == partialNextCapacity && buffer != IntPtr.Zero) {
                WriteSyntheticJobList(buffer, capacity, 2, 2, 201, 202);
                returnLength = partialFinalReturnLength;
                errorCode = 0;
                return true;
            }
            returnLength = 0;
            errorCode = ERROR_INVALID_PARAMETER;
            return false;
        };
        StartupProbeJobResult partialResult = QueryJobProcessIdsCore(new IntPtr(1), partialInvoker);
        if (!partialResult.Succeeded || partialCalls != 3 ||
            partialResult.AttemptCount != 3 || partialResult.Attempts.Length != 3 ||
            partialResult.Attempts[0].CapacityBytes != 0 ||
            partialResult.Attempts[1].CapacityBytes != 16 ||
            partialResult.Attempts[1].AssignedProcessCount != 2 ||
            partialResult.Attempts[1].ListedProcessCount != 1 ||
            !partialResult.Attempts[1].Resized ||
            partialResult.Attempts[2].CapacityBytes != partialNextCapacity ||
            partialResult.Attempts[2].AssignedProcessCount != 2 ||
            partialResult.Attempts[2].ReturnLengthBytes != partialFinalReturnLength ||
            partialResult.Attempts[2].ListedProcessCount != 2 ||
            partialResult.ProcessIds.Length != 2 ||
            partialResult.ProcessIds[0] != 201 || partialResult.ProcessIds[1] != 202) return false;

        // A retryable failure on the final permitted call must preserve that
        // native error at both the top level and the final bounded attempt.
        int exhaustedCalls = 0;
        JobQueryInvoker exhaustedInvoker = delegate(IntPtr job, IntPtr buffer, uint capacity,
            out uint returnLength, out int errorCode) {
            int call = exhaustedCalls++;
            if (call == 0 && capacity == 0) {
                returnLength = 16;
                errorCode = ERROR_INSUFFICIENT_BUFFER;
                return false;
            }
            if (call >= 1 && call < MAX_JOB_QUERY_ATTEMPT_RECORDS) {
                uint expectedCapacity = (uint)(16U << (call - 1));
                if (capacity == expectedCapacity) {
                    returnLength = 0;
                    errorCode = ERROR_MORE_DATA;
                    return false;
                }
            }
            returnLength = 0;
            errorCode = ERROR_INVALID_PARAMETER;
            return false;
        };
        StartupProbeJobResult exhaustedResult = QueryJobProcessIdsCore(new IntPtr(1), exhaustedInvoker);
        if (exhaustedCalls != MAX_JOB_QUERY_ATTEMPT_RECORDS ||
            exhaustedResult.AttemptCount != MAX_JOB_QUERY_ATTEMPT_RECORDS ||
            exhaustedResult.Attempts.Length != MAX_JOB_QUERY_ATTEMPT_RECORDS ||
            exhaustedResult.Succeeded ||
            exhaustedResult.ErrorCode != ERROR_MORE_DATA ||
            exhaustedResult.Attempts[MAX_JOB_QUERY_ATTEMPT_RECORDS - 1].ErrorCode != ERROR_MORE_DATA ||
            CanAttemptJobQuery(exhaustedCalls)) return false;

        // The shared budget predicate allows exactly eight attempts, including
        // the sizing call, and rejects both negative and exhausted counts.
        for (int attempt = 0; attempt < MAX_JOB_QUERY_ATTEMPT_RECORDS; ++attempt) {
            if (!CanAttemptJobQuery(attempt)) return false;
        }
        if (CanAttemptJobQuery(-1) || CanAttemptJobQuery(MAX_JOB_QUERY_ATTEMPT_RECORDS)) return false;
        int budgetCalls = 0;
        while (CanAttemptJobQuery(budgetCalls)) ++budgetCalls;
        if (budgetCalls != MAX_JOB_QUERY_ATTEMPT_RECORDS ||
            CanAttemptJobQuery(budgetCalls)) return false;

        // Keep the checked arithmetic and malformed invariants executable.
        if (TryGetLargerJobQueryCapacity(MAX_JOB_QUERY_BYTES, MAX_JOB_QUERY_BYTES, 0, out next) ||
            TryGetLargerJobQueryCapacity(16, 0, UInt32.MaxValue, out next)) return false;
        if (IsJobQueryCountsAndCapacityValid(32, 1, 2) ||
            IsJobQueryCountsAndCapacityValid(16, 3, 3) ||
            !IsJobQueryCountsAndCapacityValid(32, 2, 2) ||
            IsJobQueryReturnLengthValid(32, 8, 2)) return false;
        if (!IsJobQueryShapeStructurallyValid(32, 24, 2, 1) ||
            !IsJobQueryShapeStructurallyValid(16, 0, 0, 0) ||
            IsJobQueryShapeStructurallyValid(16, 0, 1, 0) ||
            IsJobQueryShapeStructurallyValid(32, 8, 2, 2)) return false;

        // A listed count larger than assigned is rejected without exposing
        // IDs, independently from the return-length and duplicate checks.
        int listedMalformedCalls = 0;
        JobQueryInvoker listedMalformedInvoker = delegate(IntPtr job, IntPtr buffer, uint capacity,
            out uint returnLength, out int errorCode) {
            int call = listedMalformedCalls++;
            if (call == 0) {
                // Two ULONG_PTR entries need 24 bytes on x64 (and still fit
                // within 24 bytes on x86); keep the malformed-count case
                // isolated without writing beyond its allocation.
                returnLength = 24;
                errorCode = ERROR_INSUFFICIENT_BUFFER;
                return false;
            }
            WriteSyntheticJobList(buffer, capacity, 1, 2, 301, 302);
            returnLength = 24;
            errorCode = 0;
            return true;
        };
        StartupProbeJobResult listedMalformedResult = QueryJobProcessIdsCore(new IntPtr(1), listedMalformedInvoker);
        if (listedMalformedCalls != 2 || listedMalformedResult.Succeeded ||
            listedMalformedResult.ErrorCode != ERROR_BAD_LENGTH ||
            listedMalformedResult.ProcessIds.Length != 0) return false;

        // A returned byte count beyond the supplied capacity is a separate
        // structural failure.
        int returnMalformedCalls = 0;
        JobQueryInvoker returnMalformedInvoker = delegate(IntPtr job, IntPtr buffer, uint capacity,
            out uint returnLength, out int errorCode) {
            int call = returnMalformedCalls++;
            if (call == 0) {
                returnLength = 16;
                errorCode = ERROR_INSUFFICIENT_BUFFER;
                return false;
            }
            WriteSyntheticJobList(buffer, capacity, 1, 1, 401, 0);
            returnLength = 24;
            errorCode = 0;
            return true;
        };
        StartupProbeJobResult returnMalformedResult = QueryJobProcessIdsCore(new IntPtr(1), returnMalformedInvoker);
        if (returnMalformedCalls != 2 || returnMalformedResult.Succeeded ||
            returnMalformedResult.ErrorCode != ERROR_BAD_LENGTH ||
            returnMalformedResult.ProcessIds.Length != 0) return false;

        // Membership can grow between successful partial responses.  This
        // drives the same production retry loop with capacities derived from
        // IntPtr.Size and the checked growth helper, rather than architecture
        // specific constants.
        uint membershipFirstRetryCapacity;
        if (!TryGetLargerJobQueryCapacity(16, 16, 2, out membershipFirstRetryCapacity) ||
            membershipFirstRetryCapacity <= 16) return false;
        uint membershipSecondRetryCapacity;
        if (!TryGetLargerJobQueryCapacity(membershipFirstRetryCapacity,
            membershipFirstRetryCapacity, 3, out membershipSecondRetryCapacity) ||
            membershipSecondRetryCapacity <= membershipFirstRetryCapacity) return false;
        uint membershipPartialReturnLength = checked(JOB_QUERY_HEADER_BYTES +
            (2U * (uint)IntPtr.Size));
        uint membershipFinalReturnLength = checked(JOB_QUERY_HEADER_BYTES +
            (3U * (uint)IntPtr.Size));
        int membershipCalls = 0;
        JobQueryInvoker membershipInvoker = delegate(IntPtr job, IntPtr buffer, uint capacity,
            out uint returnLength, out int errorCode) {
            int call = membershipCalls++;
            if (call == 0 && capacity == 0) {
                returnLength = 16;
                errorCode = ERROR_INSUFFICIENT_BUFFER;
                return false;
            }
            if (call == 1 && capacity == 16 && buffer != IntPtr.Zero) {
                WriteSyntheticJobList(buffer, capacity, 2, 1, 601, 602, 0);
                returnLength = 16;
                errorCode = 0;
                return true;
            }
            if (call == 2 && capacity == membershipFirstRetryCapacity && buffer != IntPtr.Zero) {
                WriteSyntheticJobList(buffer, capacity, 3, 2, 601, 602, 603);
                returnLength = membershipPartialReturnLength;
                errorCode = 0;
                return true;
            }
            if (call == 3 && capacity == membershipSecondRetryCapacity && buffer != IntPtr.Zero) {
                WriteSyntheticJobList(buffer, capacity, 3, 3, 601, 602, 603);
                returnLength = membershipFinalReturnLength;
                errorCode = 0;
                return true;
            }
            returnLength = 0;
            errorCode = ERROR_INVALID_PARAMETER;
            return false;
        };
        StartupProbeJobResult membershipResult = QueryJobProcessIdsCore(new IntPtr(1), membershipInvoker);
        bool membershipFinalComplete = membershipResult.Succeeded &&
            membershipResult.AttemptCount == 4 &&
            membershipResult.AssignedProcessCount == 3 &&
            membershipResult.ListedProcessCount == 3 &&
            membershipResult.ProcessIds.Length == 3 &&
            membershipResult.ProcessIds[0] == 601 &&
            membershipResult.ProcessIds[1] == 602 &&
            membershipResult.ProcessIds[2] == 603;
        if (!membershipFinalComplete || membershipCalls != 4 ||
            membershipResult.Attempts.Length != 4 ||
            membershipResult.Attempts[1].CapacityBytes != 16 ||
            membershipResult.Attempts[1].AssignedProcessCount != 2 ||
            membershipResult.Attempts[1].ListedProcessCount != 1 ||
            !membershipResult.Attempts[1].Resized ||
            membershipResult.Attempts[2].CapacityBytes != membershipFirstRetryCapacity ||
            membershipResult.Attempts[2].AssignedProcessCount != 3 ||
            membershipResult.Attempts[2].ListedProcessCount != 2 ||
            !membershipResult.Attempts[2].Resized ||
            membershipResult.Attempts[3].CapacityBytes != membershipSecondRetryCapacity ||
            membershipResult.Attempts[3].ReturnLengthBytes != membershipFinalReturnLength ||
            !membershipResult.Attempts[3].Succeeded) return false;

        // Zero, negative, and out-of-range process identifiers are all
        // rejected before any identifier array is published.
        long[] invalidProcessIds = new long[] { 0L, -1L, ((long)Int32.MaxValue) + 1L };
        for (int invalidIndex = 0; invalidIndex < invalidProcessIds.Length; ++invalidIndex) {
            long invalidProcessId = invalidProcessIds[invalidIndex];
            int invalidCalls = 0;
            JobQueryInvoker invalidPidInvoker = delegate(IntPtr job, IntPtr buffer, uint capacity,
                out uint returnLength, out int errorCode) {
                int call = invalidCalls++;
                if (call == 0 && capacity == 0) {
                    returnLength = 16;
                    errorCode = ERROR_INSUFFICIENT_BUFFER;
                    return false;
                }
                WriteSyntheticJobList(buffer, capacity, 1, 1, invalidProcessId, 0L);
                returnLength = checked(JOB_QUERY_HEADER_BYTES + (uint)IntPtr.Size);
                errorCode = 0;
                return true;
            };
            StartupProbeJobResult invalidPidResult = QueryJobProcessIdsCore(new IntPtr(1), invalidPidInvoker);
            if (invalidCalls != 2 || invalidPidResult.Succeeded ||
                invalidPidResult.ErrorCode != ERROR_BAD_LENGTH ||
                invalidPidResult.ProcessIds.Length != 0) return false;
        }
        // Duplicate positive process IDs are rejected in O(N) expected time,
        // and the partially built ID array is never published.
        int duplicateCalls = 0;
        JobQueryInvoker duplicateInvoker = delegate(IntPtr job, IntPtr buffer, uint capacity,
            out uint returnLength, out int errorCode) {
            int call = duplicateCalls++;
            if (call == 0) {
                returnLength = 24;
                errorCode = ERROR_INSUFFICIENT_BUFFER;
                return false;
            }
            WriteSyntheticJobList(buffer, capacity, 2, 2, 501, 501);
            returnLength = 24;
            errorCode = 0;
            return true;
        };
        StartupProbeJobResult duplicateResult = QueryJobProcessIdsCore(new IntPtr(1), duplicateInvoker);
        if (duplicateCalls != 2 || duplicateResult.Succeeded ||
            duplicateResult.ErrorCode != ERROR_BAD_LENGTH ||
            duplicateResult.ProcessIds.Length != 0) return false;
        return true;
    }

    private static void WriteSyntheticJobList(IntPtr buffer, uint capacity, uint assigned, uint listed,
        long firstProcessId, long secondProcessId)
    {
        WriteSyntheticJobList(buffer, capacity, assigned, listed, firstProcessId, secondProcessId, 0);
    }

    private static void WriteSyntheticJobList(IntPtr buffer, uint capacity, uint assigned, uint listed,
        long firstProcessId, long secondProcessId, long thirdProcessId)
    {
        ulong requiredBytes;
        try {
            if (buffer == IntPtr.Zero || listed > 3) throw new InvalidOperationException();
            requiredBytes = checked((ulong)JOB_QUERY_HEADER_BYTES +
                checked((ulong)listed * (ulong)IntPtr.Size));
        }
        catch {
            throw new InvalidOperationException();
        }
        if (requiredBytes > capacity) throw new InvalidOperationException();
        Marshal.WriteInt32(buffer, 0, unchecked((int)assigned));
        Marshal.WriteInt32(buffer, 4, unchecked((int)listed));
        if (listed > 0) Marshal.WriteIntPtr(buffer, (int)JOB_QUERY_HEADER_BYTES, new IntPtr(firstProcessId));
        if (listed > 1) Marshal.WriteIntPtr(buffer, (int)(JOB_QUERY_HEADER_BYTES + (uint)IntPtr.Size), new IntPtr(secondProcessId));
        if (listed > 2) Marshal.WriteIntPtr(buffer,
            checked((int)(JOB_QUERY_HEADER_BYTES + (2U * (uint)IntPtr.Size))),
            new IntPtr(thirdProcessId));
    }

    private delegate bool JobQueryInvoker(IntPtr job, IntPtr buffer, uint capacity,
        out uint returnLength, out int errorCode);

    private static bool InvokeNativeJobQuery(IntPtr job, IntPtr buffer, uint capacity,
        out uint returnLength, out int errorCode)
    {
        bool succeeded = QueryInformationJobObject(job, JobObjectBasicProcessIdList,
            buffer, capacity, out returnLength);
        errorCode = succeeded ? 0 : Marshal.GetLastWin32Error();
        return succeeded;
    }

    public static StartupProbeJobResult QueryJobProcessIds(IntPtr job)
    {
        return QueryJobProcessIdsCore(job, new JobQueryInvoker(InvokeNativeJobQuery));
    }

    private static StartupProbeJobResult QueryJobProcessIdsCore(IntPtr job, JobQueryInvoker query)
    {
        var result = new StartupProbeJobResult { Handle = job, Succeeded = false, ErrorCode = 0, ProcessIds = new int[0] };
        try {
            if (job == IntPtr.Zero || query == null) { result.ErrorCode = 6; return result; }
            uint required = 0;
            int sizingError = 0;
            bool initialSucceeded = query(job, IntPtr.Zero, 0, out required, out sizingError);
            int lastNativeError = initialSucceeded ? 0 : sizingError;
            bool lastQuerySucceeded = initialSucceeded;
            uint boundedRequired = BoundJobQueryByteCount(required);
            RecordJobQueryAttempt(result, initialSucceeded, sizingError, 0, boundedRequired, boundedRequired, 0, 0, false);
            result.RequiredBytes = boundedRequired;
            if (!initialSucceeded && !IsRetryableJobQueryError(sizingError)) {
                result.ErrorCode = sizingError;
                return result;
            }
            // The API reports ERROR_BAD_LENGTH and a zero return length for an
            // empty job on Windows.  The two ULONG header fields are followed by
            // a variable-length ULONG_PTR array; passing only the header back to
            // the second query is rejected with ERROR_BAD_LENGTH on x64.  Always
            // reserve room for the first array slot, including the empty case.
            uint minimumSize = checked((uint)(JOB_QUERY_HEADER_BYTES + (uint)IntPtr.Size));
            if (required < minimumSize) required = minimumSize;
            if (required > MAX_JOB_QUERY_BYTES) {
                // Keep the native sizing error when the query failed; the
                // bounded capacity refusal is reported as 8 only for an
                // anomalous successful sizing response.
                result.ErrorCode = initialSucceeded ? 8 : sizingError;
                return result;
            }

            // The attempt budget includes the zero-buffer sizing query above.
            // A cleanup identity-gap check may start a separate enumeration;
            // that caller-level retry has its own QueryJobProcessIds budget.
            while (CanAttemptJobQuery(result.AttemptCount)) {
                IntPtr buffer = IntPtr.Zero;
                try {
                    buffer = Marshal.AllocHGlobal(unchecked((int)required));
                    if (buffer == IntPtr.Zero) { result.ErrorCode = 8; return result; }
                    uint returned;
                    int error;
                    bool querySucceeded = query(job, buffer, required, out returned, out error);
                    if (querySucceeded) {
                        lastQuerySucceeded = true;
                    } else {
                        lastQuerySucceeded = false;
                        lastNativeError = error;
                    }
                    bool nativeByteBoundsValid = required <= MAX_JOB_QUERY_BYTES && returned <= MAX_JOB_QUERY_BYTES;
                    uint boundedReturned = BoundJobQueryByteCount(returned);
                    uint assigned = 0;
                    uint listed = 0;
                    if (querySucceeded) {
                        assigned = unchecked((uint)Marshal.ReadInt32(buffer, 0));
                        listed = unchecked((uint)Marshal.ReadInt32(buffer, 4));
                    }
                    bool resized = false;
                    uint nextRequired = 0;
                    if (!querySucceeded && IsRetryableJobQueryError(error)) {
                        resized = TryGetLargerJobQueryCapacity(required, returned, 0, out nextRequired);
                    }
                    RecordJobQueryAttempt(result, querySucceeded, error,
                        BoundJobQueryByteCount(required), BoundJobQueryByteCount(required),
                        boundedReturned, assigned, listed, resized);
                    if (!nativeByteBoundsValid) {
                        // The native result is retained as a typed failure, but
                        // no over-capacity byte value enters telemetry or the
                        // retry arithmetic.
                        result.ErrorCode = querySucceeded ? ERROR_BAD_LENGTH : error;
                        return result;
                    }
                    if (!querySucceeded) {
                        if (resized) {
                            required = nextRequired;
                            result.RequiredBytes = required;
                            continue;
                        }
                        result.ErrorCode = error;
                        return result;
                    }
                    if (!IsJobQueryShapeStructurallyValid(required, returned, assigned, listed)) {
                        result.ErrorCode = ERROR_BAD_LENGTH;
                        return result;
                    }
                    if (listed != assigned) {
                        if (!TryGetLargerJobQueryCapacity(required, returned, assigned, out nextRequired)) {
                            result.ErrorCode = ERROR_INSUFFICIENT_BUFFER;
                            return result;
                        }
                        result.Resized = true;
                        required = nextRequired;
                        result.RequiredBytes = required;
                        // Mark this successful-but-partial attempt as the source
                        // of the resize in its bounded journal entry.  The final
                        // complete attempt is the only one that can succeed.
                        if (result.Attempts != null && result.Attempts.Length > 0) {
                            result.Attempts[result.Attempts.Length - 1].Resized = true;
                        }
                        continue;
                    }
                    var ids = new int[unchecked((int)listed)];
                    // Hashtable is the non-generic O(1) set available to both
                    // Windows PowerShell 5.1 and pwsh's constrained Add-Type
                    // reference set; allocation failure is caught below.
                    var idsSeen = new Hashtable();
                    for (uint i = 0; i < listed; ++i) {
                        int offset = checked((int)(JOB_QUERY_HEADER_BYTES + checked((ulong)i * (ulong)IntPtr.Size)));
                        IntPtr value = Marshal.ReadIntPtr(buffer, offset);
                        long processId = value.ToInt64();
                        if (processId <= 0 || processId > Int32.MaxValue) { result.ErrorCode = ERROR_BAD_LENGTH; return result; }
                        int id = unchecked((int)processId);
                        if (idsSeen.ContainsKey(id)) { result.ErrorCode = ERROR_BAD_LENGTH; return result; }
                        idsSeen.Add(id, null);
                        ids[unchecked((int)i)] = id;
                    }
                    result.ProcessIds = ids;
                    result.Succeeded = true;
                    return result;
                }
                finally {
                    if (buffer != IntPtr.Zero) Marshal.FreeHGlobal(buffer);
                }
            }
            // Preserve the final native retryable error when the bounded
            // budget is exhausted.  ERROR_INSUFFICIENT_BUFFER is reserved for
            // a successful-but-partial enumeration with no larger target.
            result.ErrorCode = lastQuerySucceeded
                ? ERROR_INSUFFICIENT_BUFFER
                : lastNativeError;
            return result;
        }
        catch (OutOfMemoryException) { result.ErrorCode = 8; return result; }
        catch (OverflowException) { result.ErrorCode = ERROR_BAD_LENGTH; return result; }
        catch (Exception) { result.ErrorCode = ERROR_BAD_LENGTH; return result; }
    }

    public static StartupProbeJobResult CloseKillOnCloseJob(IntPtr job)
    {
        var result = new StartupProbeJobResult { Handle = job, Succeeded = false, ErrorCode = 0, ProcessIds = new int[0] };
        if (job == IntPtr.Zero) { result.Succeeded = true; return result; }
        if (!CloseHandle(job)) {
            result.ErrorCode = Marshal.GetLastWin32Error();
            return result;
        }
        result.Succeeded = true;
        result.Handle = IntPtr.Zero;
        return result;
    }

    private static StartupProbeAffinity ReadAffinity(IntPtr process, int processId, ulong requestedMask)
    {
        var result = new StartupProbeAffinity {
            ProcessId = processId,
            RequestedMask = requestedMask,
            ProcessMask = 0,
            SystemMask = 0,
            Opened = process != IntPtr.Zero,
            SetSucceeded = requestedMask == 0,
            ReadBackSucceeded = false,
            Verified = false,
            DescendantsVerified = true,
            ErrorCode = 0
        };
        UIntPtr processMask;
        UIntPtr systemMask;
        if (!GetProcessAffinityMask(process, out processMask, out systemMask)) {
            result.ErrorCode = Marshal.GetLastWin32Error();
            return result;
        }
        result.ProcessMask = processMask.ToUInt64();
        result.SystemMask = systemMask.ToUInt64();
        result.ReadBackSucceeded = true;
        result.Verified = requestedMask == 0 || result.ProcessMask == requestedMask;
        return result;
    }

    public static StartupProbeAffinity SetAndReadProcessAffinity(int processId, ulong requestedMask)
    {
        var result = new StartupProbeAffinity {
            ProcessId = processId,
            RequestedMask = requestedMask,
            ProcessMask = 0,
            SystemMask = 0,
            Opened = false,
            SetSucceeded = false,
            ReadBackSucceeded = false,
            Verified = false,
            DescendantsVerified = true,
            ErrorCode = 87
        };
        if (requestedMask == 0) return result;
        IntPtr process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_INFORMATION, false, unchecked((uint)processId));
        if (process == IntPtr.Zero) {
            result.ErrorCode = Marshal.GetLastWin32Error();
            return result;
        }
        try {
            result.Opened = true;
            UIntPtr processMask;
            UIntPtr systemMask;
            if (!GetProcessAffinityMask(process, out processMask, out systemMask)) {
                result.ErrorCode = Marshal.GetLastWin32Error();
                return result;
            }
            result.ProcessMask = processMask.ToUInt64();
            result.SystemMask = systemMask.ToUInt64();
            if ((requestedMask & ~result.SystemMask) != 0) {
                result.ErrorCode = 87;
                return result;
            }
            if (!SetProcessAffinityMask(process, new UIntPtr(requestedMask))) {
                result.ErrorCode = Marshal.GetLastWin32Error();
                return result;
            }
            result.SetSucceeded = true;
            var readBack = ReadAffinity(process, processId, requestedMask);
            result.ProcessMask = readBack.ProcessMask;
            result.SystemMask = readBack.SystemMask;
            result.ReadBackSucceeded = readBack.ReadBackSucceeded;
            result.Verified = readBack.Verified;
            result.DescendantsVerified = readBack.DescendantsVerified;
            result.ErrorCode = readBack.ErrorCode;
            return result;
        }
        finally { CloseHandle(process); }
    }

    public static StartupProbeAffinity SetAndReadProcessAffinityHandle(IntPtr process, int processId, ulong requestedMask)
    {
        var result = new StartupProbeAffinity {
            ProcessId = processId,
            RequestedMask = requestedMask,
            ProcessMask = 0,
            SystemMask = 0,
            Opened = process != IntPtr.Zero,
            SetSucceeded = false,
            ReadBackSucceeded = false,
            Verified = false,
            DescendantsVerified = false,
            ErrorCode = 87
        };
        if (process == IntPtr.Zero || processId <= 0 || requestedMask == 0) return result;

        UIntPtr processMask;
        UIntPtr systemMask;
        if (!GetProcessAffinityMask(process, out processMask, out systemMask)) {
            result.ErrorCode = Marshal.GetLastWin32Error();
            return result;
        }
        result.ProcessMask = processMask.ToUInt64();
        result.SystemMask = systemMask.ToUInt64();
        if ((requestedMask & ~result.SystemMask) != 0) {
            result.ErrorCode = 87;
            return result;
        }
        if (!SetProcessAffinityMask(process, new UIntPtr(requestedMask))) {
            result.ErrorCode = Marshal.GetLastWin32Error();
            return result;
        }
        result.SetSucceeded = true;
        var readBack = ReadAffinity(process, processId, requestedMask);
        result.ProcessMask = readBack.ProcessMask;
        result.SystemMask = readBack.SystemMask;
        result.ReadBackSucceeded = readBack.ReadBackSucceeded;
        result.Verified = readBack.Verified;
        result.ErrorCode = readBack.ErrorCode;
        return result;
    }

    public static StartupProbeAffinity ReadProcessAffinity(int processId, ulong expectedMask)
    {
        var result = new StartupProbeAffinity {
            ProcessId = processId,
            RequestedMask = expectedMask,
            ProcessMask = 0,
            SystemMask = 0,
            Opened = false,
            SetSucceeded = false,
            ReadBackSucceeded = false,
            Verified = false,
            DescendantsVerified = true,
            ErrorCode = 0
        };
        IntPtr process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION, false, unchecked((uint)processId));
        if (process == IntPtr.Zero) {
            result.ErrorCode = Marshal.GetLastWin32Error();
            return result;
        }
        try {
            result = ReadAffinity(process, processId, expectedMask);
            return result;
        }
        finally { CloseHandle(process); }
    }

    public static bool RequestClose(IntPtr handle)
    {
        return PostMessage(handle, WM_CLOSE, IntPtr.Zero, IntPtr.Zero);
    }

    public static bool SaveWindowPng(IntPtr handle, string outputPath)
    {
        RECT rect;
        if (!GetWindowRect(handle, out rect)) return false;
        int width = rect.Right - rect.Left;
        int height = rect.Bottom - rect.Top;
        if (width <= 0 || height <= 0) return false;
        using (var bitmap = new Bitmap(width, height, PixelFormat.Format32bppArgb)) {
            using (var graphics = Graphics.FromImage(bitmap)) {
                graphics.CopyFromScreen(rect.Left, rect.Top, 0, 0,
                    new Size(width, height), CopyPixelOperation.SourceCopy);
            }
            bitmap.Save(outputPath, ImageFormat.Png);
            return true;
        }
    }
}
'@ -ReferencedAssemblies $startupProbeReferences.ToArray()

function Get-NormalizedPath([string]$Path) {
    return [IO.Path]::GetFullPath($Path).TrimEnd('\\').ToUpperInvariant()
}

function Test-SamePath([string]$Left, [string]$Right) {
    return (Get-NormalizedPath $Left) -eq (Get-NormalizedPath $Right)
}

function Assert-AffinityMask([UInt64]$Mask) {
    if ($Mask -eq 0) { throw 'Affinity mask must be nonzero.' }
    return $Mask
}

function Add-StartupSaturatingCount {
    param(
        [AllowNull()] [object]$Value,
        [AllowNull()] [object]$Increment = 1,
        [int]$Maximum = 4096
    )
    try {
        $current = if ($null -eq $Value) { [decimal]0 } else { [decimal]$Value }
        $delta = if ($null -eq $Increment) { [decimal]0 } else { [decimal]$Increment }
        if ($current -lt 0) { $current = 0 }
        if ($delta -lt 0) { $delta = 0 }
        $limit = [decimal][Math]::Max(0, $Maximum)
        $total = $current + $delta
        if ($total -gt $limit) { return [int]$limit }
        return [int]$total
    }
    catch { return 0 }
}

function Convert-StartupObservationErrorCode {
    param([AllowNull()] [object]$Value)
    if ($null -eq $Value -or $Value -is [bool]) { return $null }
    try {
        $number = [decimal]$Value
        if ($number -lt 0 -or $number -gt [decimal][int]::MaxValue -or
            $number -ne [decimal]::Truncate($number)) { return $null }
        return [int]$number
    }
    catch { return $null }
}

function Test-StartupDiagnosticFailureSubstage {
    param([AllowNull()] [object]$Value)
    if ($Value -isnot [string]) { return $false }
    foreach ($allowed in @($startupDiagnosticFailureSubstages)) {
        if ([StringComparer]::Ordinal.Equals([string]$Value, [string]$allowed)) { return $true }
    }
    return $false
}

function Set-StartupDiagnosticPhase {
    param(
        [AllowNull()] [object]$Phase,
        [Parameter(Mandatory = $true)] [string]$Substage,
        [AllowNull()] [object]$ErrorCode = $null
    )
    if ($null -eq $Phase) { return }
    $Phase.substage = $Substage
    $normalized = Convert-StartupObservationErrorCode $ErrorCode
    $Phase.errorCode = if ($null -eq $normalized -or $normalized -eq 0) { $null } else { [int]$normalized }
}

function New-StartupDiagnosticProcessObservation {
    return [ordered]@{
        processEnumerationAttempted = $false
        processEnumerationSucceeded = $false
        processEnumerationComplete = $false
        processEnumerationErrorCode = $null
        processEnumerationRetryCount = 0
        processEnumerationCallCount = 0
        processEnumerationCompletedCount = 0
        processEnumerationFailureCount = 0
    }
}

function Get-StartupAffinityFailureType {
    param([AllowNull()] [object]$Probe)
    if ($null -eq $Probe) { return 'unavailable' }
    try {
        if (-not [bool]$Probe.Opened) { return 'open' }
        if (-not [bool]$Probe.SetSucceeded) { return 'set' }
        if (-not [bool]$Probe.ReadBackSucceeded) { return 'readback' }
        if (-not [bool]$Probe.Verified) { return 'mismatch' }
        return 'none'
    }
    catch { return 'unavailable' }
}

function Get-StartupAffinityFailureErrorCode {
    param(
        [AllowNull()] [object]$Probe,
        [AllowNull()] [object]$FailureType = $null
    )
    $type = if ($null -eq $FailureType) { Get-StartupAffinityFailureType $Probe } else { [string]$FailureType }
    if ($type -eq 'none') { return $null }
    $code = if ($null -eq $Probe) { $null } else { Convert-StartupObservationErrorCode (Get-StartupObservationProperty $Probe 'ErrorCode') }
    if ($null -eq $code -or $code -eq 0) { return 13 }
    return [int]$code
}

function Get-AffinityMetadata($Probe) {
    $failureType = Get-StartupAffinityFailureType $Probe
    return [ordered]@{
        requestedMask = [UInt64]$Probe.RequestedMask
        processMask = [UInt64]$Probe.ProcessMask
        systemMask = [UInt64]$Probe.SystemMask
        opened = [bool]$Probe.Opened
        setSucceeded = [bool]$Probe.SetSucceeded
        readBackSucceeded = [bool]$Probe.ReadBackSucceeded
        verified = [bool]$Probe.Verified
        descendantsVerified = [bool]$Probe.DescendantsVerified
        errorCode = [int]$Probe.ErrorCode
        historicalOwnedCount = [int]0
        currentLiveCount = [int]0
        expiredHistoricalCount = [int]0
        failureType = $failureType
        failureErrorCode = Get-StartupAffinityFailureErrorCode $Probe $failureType
        liveSetSource = 'not-attempted'
    }
}

function Set-StartupAffinityLiveSetMetadata {
    param(
        [Parameter(Mandatory = $true)] [object]$Metadata,
        [AllowNull()] [object]$HistoricalCount = 0,
        [AllowNull()] [object]$CurrentCount = 0,
        [AllowNull()] [object]$Source = 'unavailable'
    )
    try {
        $historical = Add-StartupSaturatingCount $HistoricalCount 0 $startupObservationMaxCount
        $current = Add-StartupSaturatingCount $CurrentCount 0 $startupObservationMaxCount
        if ($current -gt $historical) { $current = $historical }
        $Metadata.historicalOwnedCount = $historical
        $Metadata.currentLiveCount = $current
        $Metadata.expiredHistoricalCount = Add-StartupSaturatingCount ($historical - $current) 0 $startupObservationMaxCount
        $sourceText = [string]$Source
        $Metadata.liveSetSource = if ($startupAffinityLiveSetSources -contains $sourceText) { $sourceText } else { 'unavailable' }
    }
    catch {
        $Metadata.historicalOwnedCount = 0
        $Metadata.currentLiveCount = 0
        $Metadata.expiredHistoricalCount = 0
        $Metadata.liveSetSource = 'unavailable'
    }
    return $Metadata
}

function Get-StartupAffinityVerificationPlan {
    param(
        [AllowNull()] [object]$HistoricalRecords,
        [AllowNull()] [object]$CurrentRecords
    )
    if ($null -eq $HistoricalRecords -or -not ($HistoricalRecords -is [Collections.IDictionary])) {
        throw 'The historical affinity process set is unavailable.'
    }
    if ($null -eq $CurrentRecords) {
        throw 'The current-live affinity process set is unavailable.'
    }

    $historicalById = @{}
    foreach ($historicalEntry in @($HistoricalRecords.GetEnumerator())) {
        if ($null -eq $historicalEntry -or $null -eq $historicalEntry.Value) {
            throw 'The historical affinity process set contains an unavailable record.'
        }
        $historicalIdValue = Get-StartupObservationProperty $historicalEntry.Value 'Id'
        if ($null -eq $historicalIdValue -or $historicalIdValue -is [bool]) {
            throw 'The historical affinity process set contains a malformed process identity.'
        }
        try { $historicalId = [int]$historicalIdValue } catch { throw 'The historical affinity process set contains a malformed process identity.' }
        if ($historicalId -le 0 -or $historicalById.ContainsKey($historicalId)) {
            throw 'The historical affinity process set contains a duplicate or invalid process identity.'
        }
        try { $historicalKey = [int]$historicalEntry.Key } catch { throw 'The historical affinity process set key is malformed.' }
        if ($historicalKey -ne $historicalId) {
            throw "The historical affinity process set key does not match process $historicalId."
        }
        $historicalCreationValue = Get-StartupObservationProperty $historicalEntry.Value 'Creation'
        $historicalPath = Get-StartupObservationProperty $historicalEntry.Value 'ImagePath'
        if ($null -eq $historicalCreationValue -or [string]::IsNullOrWhiteSpace([string]$historicalPath)) {
            throw "The historical affinity process $historicalId has an incomplete identity."
        }
        try { $historicalCreation = [long]$historicalCreationValue } catch { throw "The historical affinity process $historicalId has a malformed creation identity." }
        $historicalById[$historicalId] = [pscustomobject][ordered]@{
            record = $historicalEntry.Value
            creation = $historicalCreation
            imagePath = [string]$historicalPath
        }
    }
    if ($historicalById.Count -eq 0) {
        throw 'The historical affinity process set is empty.'
    }

    $currentRecordsArray = @($CurrentRecords)
    if ($currentRecordsArray.Count -eq 0) {
        throw 'The current-live affinity process set is empty.'
    }
    $currentById = @{}
    $plan = New-Object Collections.Generic.List[object]
    foreach ($current in $currentRecordsArray) {
        if ($null -eq $current) {
            throw 'The current-live affinity process set contains an unavailable record.'
        }
        $currentIdValue = Get-StartupObservationProperty $current 'Id'
        if ($null -eq $currentIdValue -or $currentIdValue -is [bool]) {
            throw 'The current-live affinity process set contains a malformed process identity.'
        }
        try { $currentId = [int]$currentIdValue } catch { throw 'The current-live affinity process set contains a malformed process identity.' }
        if ($currentId -le 0) {
            throw 'The current-live affinity process set contains an invalid process identity.'
        }
        if ($currentById.ContainsKey($currentId)) {
            throw "The current-live affinity process set contains duplicate process $currentId."
        }
        if (-not $historicalById.ContainsKey($currentId)) {
            throw "The current-live affinity process set contains unknown process $currentId."
        }
        $historical = $historicalById[$currentId]
        $currentCreationValue = Get-StartupObservationProperty $current 'Creation'
        $currentPath = Get-StartupObservationProperty $current 'ImagePath'
        if ($null -eq $currentCreationValue -or [string]::IsNullOrWhiteSpace([string]$currentPath)) {
            throw "The current-live affinity process $currentId has an incomplete identity."
        }
        try { $currentCreation = [long]$currentCreationValue } catch { throw "The current-live affinity process $currentId has a malformed creation identity." }
        if ($historical.creation -ne $currentCreation -or -not (Test-SamePath $currentPath $historical.imagePath)) {
            throw "The current-live affinity process $currentId does not match its historical identity."
        }
        $currentById[$currentId] = $true
        [void]$plan.Add($current)
    }

    # Validate the complete current set before performing any native read-back.
    # This prevents malformed, duplicate, unknown, or identity-reused records
    # from producing a partial affinity result.
    return $plan.ToArray()
}

function Set-ProcessAffinityVerified([int]$ProcessId, [UInt64]$Mask) {
    [void](Assert-AffinityMask $Mask)
    $probe = [NativeStartupProbe]::SetAndReadProcessAffinity($ProcessId, $Mask)
    $metadata = Get-AffinityMetadata $probe
    if (-not $metadata.opened -or -not $metadata.setSucceeded -or
        -not $metadata.readBackSucceeded -or -not $metadata.verified) {
        throw 'Process affinity could not be applied and read back exactly.'
    }
    return $metadata
}

function Set-ProcessAffinityHandleVerified([IntPtr]$ProcessHandle, [int]$ProcessId, [UInt64]$Mask) {
    [void](Assert-AffinityMask $Mask)
    $probe = [NativeStartupProbe]::SetAndReadProcessAffinityHandle($ProcessHandle, $ProcessId, $Mask)
    $metadata = Get-AffinityMetadata $probe
    if (-not $metadata.opened -or -not $metadata.setSucceeded -or
        -not $metadata.readBackSucceeded -or -not $metadata.verified) {
        throw 'Process affinity could not be applied to the suspended process and read back exactly.'
    }
    $metadata.descendantsVerified = $false
    return $metadata
}

function Read-ProcessAffinityVerified([int]$ProcessId, [UInt64]$Mask) {
    [void](Assert-AffinityMask $Mask)
    $probe = [NativeStartupProbe]::ReadProcessAffinity($ProcessId, $Mask)
    $metadata = Get-AffinityMetadata $probe
    if (-not $metadata.opened -or -not $metadata.readBackSucceeded -or -not $metadata.verified) {
        throw 'Run-owned process affinity did not match the requested mask.'
    }
    return $metadata
}

function Convert-ProcessIdentity($Process) {
    return [pscustomobject]@{
        Id = [int]$Process.ProcessId
        ParentId = [int]$Process.ParentProcessId
        Creation = [long]$Process.CreationTime
        ImagePath = [string]$Process.ImagePath
    }
}

function New-StartupJobIdentityObservation {
    return [ordered]@{
        identityAttemptCount = 0
        identityFailureCount = 0
        recoveryAttemptCount = 0
        disappearedAfterSnapshotCount = 0
        stillPresentAfterFailureCount = 0
        failureType = 'none'
        failureErrorCode = $null
        failureOperation = 'none'
        observerRole = 'none'
    }
}

function New-StartupJobQueryObservation {
    return [ordered]@{
        attempted = $false
        skipped = $false
        succeeded = $false
        errorCode = $null
        queryCount = 0
        attemptCount = 0
        capacityBytes = [UInt64]0
        requiredBytes = [UInt64]0
        returnLengthBytes = [UInt64]0
        assignedProcessCount = [UInt64]0
        listedProcessCount = [UInt64]0
        resized = $false
        attempts = @()
        attemptsTruncated = $false
    }
}

function New-StartupContainmentProof {
    return [ordered]@{
        version = 2
        mode = 'unavailable'
        terminalState = 'not-attempted'
        terminationAttempted = $false
        terminationSucceeded = $false
        terminationErrorCode = $null
        terminalJobQuery = New-StartupJobQueryObservation
        terminalJobMemberCount = $null
        jobEmptyProven = $false
        identityReconciliation = [ordered]@{
            attempted = $false
            accepted = $false
            operation = 'none'
            observerRole = 'none'
            reason = 'none'
        }
    }
}

function New-StartupCleanupObservation {
    return [ordered]@{
        attempted = $false
        jobPresent = $false
        jobQueryAttempted = $false
        jobQuerySkipped = $false
        jobQuerySucceeded = $false
        query = New-StartupJobQueryObservation
        jobCloseAttempted = $false
        jobCloseSucceeded = $false
        gracefulCloseAttempted = $false
        gracefulCloseSucceeded = $false
        gracefulCloseFallbackType = 'none'
        trackedSweepAttempted = $false
        trackedSweepVerified = $false
        finalPathSweepAttempted = $false
        finalPathSweepVerified = $false
        survivorCount = 0
        cleanupErrorCount = 0
        # One aggregate covers every typed process census made by cleanup,
        # including tracked and executable-path sweeps.  A successful aggregate
        # means every observed census call completed; the first failed call's
        # error is retained even when a later call succeeds.
        processEnumerationAttempted = $false
        processEnumerationSucceeded = $false
        processEnumerationComplete = $false
        processEnumerationErrorCode = $null
        processEnumerationRetryCount = 0
        processEnumerationCallCount = 0
        processEnumerationCompletedCount = 0
        processEnumerationFailureCount = 0
        # Job-membership identity races are diagnostic for the Job path and
        # intentionally do not reuse tracked-sweep-specific counters.
        jobIdentityObservation = New-StartupJobIdentityObservation
        containmentProof = New-StartupContainmentProof
        trackedSweepFailureType = 'none'
        trackedSweepFailureErrorCode = $null
        trackedSweepIdentityAttemptCount = 0
        trackedSweepIdentityFailureCount = 0
        trackedSweepDisappearedAfterSnapshotCount = 0
        trackedSweepStillPresentAfterFailureCount = 0
        trackedSweepPassCount = 0
    }
}

function Set-StartupIdentityReconciliation {
    param(
        [AllowNull()] [object]$ContainmentProof,
        [AllowNull()] [object]$Operation,
        [AllowNull()] [object]$ObserverRole
    )
    if ($null -eq $ContainmentProof) { return $false }
    $reconciliation = Get-StartupObservationProperty $ContainmentProof 'identityReconciliation'
    if ($null -eq $reconciliation) { return $false }
    $operationValue = [string]$Operation
    $observerRoleValue = [string]$ObserverRole
    $operationAllowlisted = $startupIdentityOperations -contains $operationValue
    $observerRoleAllowlisted = $startupIdentityObserverRoles -contains $observerRoleValue
    $reconciliation.attempted = $true
    $reconciliation.accepted = $false
    $reconciliation.operation = if ($operationAllowlisted) { $operationValue } else { 'exception' }
    $reconciliation.observerRole = if ($observerRoleAllowlisted) { $observerRoleValue } else { 'none' }
    if ($observerRoleValue -eq 'exact-path') {
        $reconciliation.reason = 'exact-path-failure'
        return $false
    }
    if (-not [bool](Get-StartupObservationProperty $ContainmentProof 'jobEmptyProven')) {
        $reconciliation.reason = 'job-empty-not-proven'
        return $false
    }
    if (-not $operationAllowlisted -or
        $startupIdentityReconciliationOperations -notcontains $operationValue) {
        $reconciliation.reason = 'operation-not-allowlisted'
        return $false
    }
    if (-not $observerRoleAllowlisted -or
        $startupIdentityReconciliationObserverRoles -notcontains $observerRoleValue) {
        $reconciliation.reason = 'observer-not-allowlisted'
        return $false
    }
    $reconciliation.accepted = $true
    $reconciliation.reason = 'job-empty-and-allowlisted-post-close-history'
    return $true
}

function Get-StartupObservationProperty {
    param(
        [AllowNull()] [object]$Object,
        [Parameter(Mandatory = $true)] [string]$Name,
        [AllowNull()] [object]$Default = $null
    )
    try {
        if ($null -ne $Object) {
            if ($Object -is [Collections.IDictionary]) {
                if ($Object.Contains($Name)) { return $Object[$Name] }
            }
            else {
                $property = $Object.PSObject.Properties[$Name]
                if ($null -ne $property) { return $property.Value }
            }
        }
    }
    catch { }
    return $Default
}

function Test-StartupObservationPropertyPresent {
    param(
        [AllowNull()] [object]$Object,
        [Parameter(Mandatory = $true)] [string]$Name
    )
    if ($null -eq $Object) { return $false }
    try {
        if ($Object -is [Collections.IDictionary]) { return [bool]$Object.Contains($Name) }
        return $null -ne $Object.PSObject.Properties[$Name]
    }
    catch { return $false }
}

function Test-StartupIntegralValue {
    param(
        [AllowNull()] [object]$Value,
        [Parameter(Mandatory = $true)] [decimal]$Minimum,
        [Parameter(Mandatory = $true)] [decimal]$Maximum
    )
    if ($null -eq $Value -or $Value -is [bool] -or
        ($Value -isnot [byte] -and $Value -isnot [sbyte] -and
         $Value -isnot [int16] -and $Value -isnot [uint16] -and
         $Value -isnot [int32] -and $Value -isnot [uint32] -and
         $Value -isnot [int64] -and $Value -isnot [uint64])) {
        return $false
    }
    try {
        $number = [decimal]$Value
        return $number -ge $Minimum -and $number -le $Maximum -and
            $number -eq [decimal]::Truncate($number)
    }
    catch { return $false }
}

function Convert-StartupObservationInt([AllowNull()] [object]$Value) {
    if ($null -eq $Value) { return 0 }
    try { return [int]$Value } catch { return 0 }
}

function Convert-StartupObservationUInt64([AllowNull()] [object]$Value) {
    if ($null -eq $Value) { return [UInt64]0 }
    try {
        $number = [decimal]$Value
        if ($number -lt 0 -or $number -gt [decimal][UInt64]::MaxValue) { return [UInt64]0 }
        return [UInt64]$number
    }
    catch { return [UInt64]0 }
}

function Convert-StartupObservationBool([AllowNull()] [object]$Value) {
    if ($null -eq $Value) { return $false }
    try { return [bool]$Value } catch { return $false }
}

function Add-StartupProcessEnumerationObservation {
    param(
        [Parameter(Mandatory = $true)] [object]$Target,
        [AllowNull()] [object]$Probe
    )
    if ($null -eq $Probe) { return }
    try {
        $attempted = Convert-StartupObservationBool (Get-StartupObservationProperty $Probe 'Attempted')
        $complete = Convert-StartupObservationBool (Get-StartupObservationProperty $Probe 'Complete')
        $succeeded = Convert-StartupObservationBool (Get-StartupObservationProperty $Probe 'Succeeded')
        $retryCount = Convert-StartupObservationInt (Get-StartupObservationProperty $Probe 'RetryCount')
        if ($retryCount -lt 0) { $retryCount = 0 }
        $errorCode = Convert-StartupObservationErrorCode (Get-StartupObservationProperty $Probe 'ErrorCode')
        $callCount = Convert-StartupObservationInt (Get-StartupObservationProperty $Target 'processEnumerationCallCount')
        if ($callCount -lt 0) { $callCount = 0 }
        $isFirst = $callCount -eq 0
        $Target.processEnumerationCallCount = Add-StartupSaturatingCount $callCount 1 $startupObservationMaxCount
        $Target.processEnumerationRetryCount = Add-StartupSaturatingCount (Get-StartupObservationProperty $Target 'processEnumerationRetryCount') $retryCount $startupObservationMaxCount
        # A typed result is evidence that a census call was made even when the
        # probe reports Attempted=false.  The aggregate remains incomplete in
        # that case, so this cannot make a failed call look successful.
        $Target.processEnumerationAttempted = $true
        if ($complete -and $succeeded) {
            $Target.processEnumerationCompletedCount = Add-StartupSaturatingCount (Get-StartupObservationProperty $Target 'processEnumerationCompletedCount') 1 $startupObservationMaxCount
        }
        else {
            $Target.processEnumerationFailureCount = Add-StartupSaturatingCount (Get-StartupObservationProperty $Target 'processEnumerationFailureCount') 1 $startupObservationMaxCount
            $existingError = Convert-StartupObservationErrorCode (Get-StartupObservationProperty $Target 'processEnumerationErrorCode')
            if ($null -eq $existingError -or $existingError -eq 0) {
                $Target.processEnumerationErrorCode = if ($null -eq $errorCode -or $errorCode -eq 0) { 13 } else { [int]$errorCode }
            }
        }
        if ($isFirst) {
            $Target.processEnumerationSucceeded = [bool]($attempted -and $complete -and $succeeded)
            $Target.processEnumerationComplete = [bool]($attempted -and $complete)
        }
        else {
            $Target.processEnumerationSucceeded = [bool](Convert-StartupObservationBool (Get-StartupObservationProperty $Target 'processEnumerationSucceeded')) -and
                [bool]($attempted -and $complete -and $succeeded)
            $Target.processEnumerationComplete = [bool](Convert-StartupObservationBool (Get-StartupObservationProperty $Target 'processEnumerationComplete')) -and
                [bool]($attempted -and $complete)
        }
    }
    catch { }
}

function Set-StartupTrackedSweepFailure {
    param(
        [AllowNull()] [object]$Observation,
        [AllowNull()] [object]$FailureType,
        [AllowNull()] [object]$ErrorCode
    )
    if ($null -eq $Observation) { return }
    try {
        $candidateType = [string]$FailureType
        if ($candidateType -eq 'none' -or $startupTrackedSweepFailureTypes -notcontains $candidateType) {
            $candidateType = 'exception'
        }
        $existingType = [string](Get-StartupObservationProperty $Observation 'trackedSweepFailureType')
        if ($startupTrackedSweepFailureTypes -notcontains $existingType) { $existingType = 'none' }
        if ($existingType -eq 'none') { $Observation.trackedSweepFailureType = $candidateType }

        $candidateError = Convert-StartupObservationErrorCode $ErrorCode
        if ($null -eq $candidateError -or $candidateError -eq 0) { $candidateError = 13 }
        $existingError = Convert-StartupObservationErrorCode (Get-StartupObservationProperty $Observation 'trackedSweepFailureErrorCode')
        if ($null -eq $existingError -or $existingError -eq 0) {
            $Observation.trackedSweepFailureErrorCode = [int]$candidateError
        }
    }
    catch { }
}

function Add-StartupTrackedSweepCount {
    param(
        [Parameter(Mandatory = $true)] [object]$Observation,
        [Parameter(Mandatory = $true)] [string]$Field,
        [int]$Increment = 1
    )
    if ($startupTrackedSweepCounterFields -notcontains $Field) {
        throw "Unsupported tracked-sweep counter field: $Field"
    }
    try {
        $Observation.$Field = Add-StartupSaturatingCount (Get-StartupObservationProperty $Observation $Field) $Increment $startupObservationMaxCount
    }
    catch { }
}

function Add-StartupJobIdentityCount {
    param(
        [AllowNull()] [object]$Observation,
        [Parameter(Mandatory = $true)] [string]$Field,
        [int]$Increment = 1
    )
    if ($startupJobIdentityCounterFields -notcontains $Field) {
        throw "Unsupported Job identity counter field: $Field"
    }
    if ($null -eq $Observation) { return }
    try {
        $Observation.$Field = Add-StartupSaturatingCount (Get-StartupObservationProperty $Observation $Field) $Increment $startupObservationMaxCount
    }
    catch { }
}

function Set-StartupJobIdentityFailure {
    param(
        [AllowNull()] [object]$Observation,
        [AllowNull()] [object]$FailureType,
        [AllowNull()] [object]$ErrorCode,
        [AllowNull()] [object]$Operation = 'exception',
        [AllowNull()] [object]$ObserverRole = 'launch-job-member'
    )
    if ($null -eq $Observation) { return }
    try {
        $candidateType = [string]$FailureType
        if ($candidateType -eq 'none' -or $startupTrackedSweepFailureTypes -notcontains $candidateType) {
            $candidateType = 'exception'
        }
        $existingType = [string](Get-StartupObservationProperty $Observation 'failureType')
        if ($startupTrackedSweepFailureTypes -notcontains $existingType) { $existingType = 'none' }
        # Preserve the first typed failure and its first-cause error code.  A
        # later member must not overwrite the race that started this recovery.
        if ($existingType -eq 'none') {
            $Observation.failureType = $candidateType
            $operationValue = [string]$Operation
            $observerRoleValue = [string]$ObserverRole
            $Observation.failureOperation = if ($startupIdentityOperations -contains $operationValue) { $operationValue } else { 'exception' }
            $Observation.observerRole = if ($startupIdentityObserverRoles -contains $observerRoleValue) { $observerRoleValue } else { 'none' }
        }

        $candidateError = Convert-StartupObservationErrorCode $ErrorCode
        if ($null -eq $candidateError -or $candidateError -eq 0) { $candidateError = 13 }
        $existingError = Convert-StartupObservationErrorCode (Get-StartupObservationProperty $Observation 'failureErrorCode')
        if ($null -eq $existingError -or $existingError -eq 0) {
            $Observation.failureErrorCode = [int]$candidateError
        }
    }
    catch { }
}

function Get-StartupTrackedSweepFailureType {
    param(
        [Parameter(Mandatory = $true)] [int]$ProcessId,
        [AllowNull()] [object]$FreshProbe
    )
    try {
        if ($ProcessId -le 0) { return 'enumeration-unavailable' }
        $attemptedValue = Get-StartupObservationProperty $FreshProbe 'Attempted'
        $completeValue = Get-StartupObservationProperty $FreshProbe 'Complete'
        $succeededValue = Get-StartupObservationProperty $FreshProbe 'Succeeded'
        # Read Entries directly so assignment does not run the value through
        # PowerShell's pipeline unrolling.  The raw property must itself be an
        # Array; a scalar forged entry is never sufficient proof of absence.
        $entriesValue = $null
        if ($null -ne $FreshProbe) {
            if ($FreshProbe -is [Collections.IDictionary]) {
                if ($FreshProbe.Contains('Entries')) { $entriesValue = $FreshProbe['Entries'] }
            }
            else {
                $entriesProperty = $FreshProbe.PSObject.Properties['Entries']
                if ($null -ne $entriesProperty) { $entriesValue = $entriesProperty.Value }
            }
        }
        if ($null -eq $FreshProbe -or
            $attemptedValue -isnot [bool] -or -not $attemptedValue -or
            $completeValue -isnot [bool] -or -not $completeValue -or
            $succeededValue -isnot [bool] -or -not $succeededValue -or
            $null -eq $entriesValue -or $entriesValue -isnot [Array] -or
            $entriesValue.Length -gt 65536) {
            return 'enumeration-unavailable'
        }
        $attemptCountValue = Get-StartupObservationProperty $FreshProbe 'AttemptCount'
        if ($attemptCountValue -isnot [int32]) { return 'enumeration-unavailable' }
        $attemptCount = [int]$attemptCountValue
        if ($attemptCount -lt 1 -or $attemptCount -gt 3) { return 'enumeration-unavailable' }
        $retryCountValue = Get-StartupObservationProperty $FreshProbe 'RetryCount'
        if ($retryCountValue -isnot [int32]) { return 'enumeration-unavailable' }
        $retryCount = [int]$retryCountValue
        if ($retryCount -lt 0 -or $retryCount -ge $attemptCount) { return 'enumeration-unavailable' }
        $retriedValue = Get-StartupObservationProperty $FreshProbe 'Retried'
        if ($retriedValue -isnot [bool] -or [bool]$retriedValue -ne ($retryCount -gt 0)) {
            return 'enumeration-unavailable'
        }
        $errorValue = Get-StartupObservationProperty $FreshProbe 'ErrorCode'
        if ($errorValue -isnot [int32] -or [int]$errorValue -ne 0) {
            return 'enumeration-unavailable'
        }
        $seen = @{}
        $containsTarget = $false
        foreach ($entry in $entriesValue) {
            if ($null -eq $entry) { return 'exception' }
            $entryIdValue = Get-StartupObservationProperty $entry 'ProcessId'
            $parentIdValue = Get-StartupObservationProperty $entry 'ParentProcessId'
            $imageNameValue = Get-StartupObservationProperty $entry 'ImageName'
            if (-not (Test-StartupIntegralValue $entryIdValue 0 ([decimal][int]::MaxValue)) -or
                -not (Test-StartupIntegralValue $parentIdValue 0 ([decimal][int]::MaxValue)) -or
                $imageNameValue -isnot [string] -or [string]::IsNullOrWhiteSpace($imageNameValue) -or
                $imageNameValue.Length -ge 260) {
                return 'exception'
            }
            $entryId = [int]$entryIdValue
            $parentId = [int]$parentIdValue
            # PID 0 is the valid Windows System Idle Process entry.  The
            # tracked process itself is always positive, but the fresh native
            # census may legitimately contain this zero-PID record.
            if ($entryId -lt 0 -or $parentId -lt 0 -or $seen.ContainsKey($entryId)) {
                return 'exception'
            }
            $seen[$entryId] = $true
            if ($entryId -eq $ProcessId) { $containsTarget = $true }
        }
        if ($containsTarget) { return 'identity-still-present' }
        return 'identity-disappeared'
    }
    catch { return 'exception' }
}

function Invoke-StartupTrackedIdentityFailure {
    param(
        [AllowNull()] [object]$CleanupObservation,
        [Parameter(Mandatory = $true)] [int]$ProcessId,
        [AllowNull()] [object]$IdentityProbe,
        [AllowNull()] [scriptblock]$FreshCensus = $null,
        [Parameter(Mandatory = $true)] [string]$ErrorMessage,
        [bool]$RecordTrackedSweepTelemetry = $true,
        [string]$ObserverRole = 'none',
        [AllowNull()] [object]$ContainmentProof = $null
    )
    # Recovery is limited to a coherent typed QueryProcessIdentity failure.
    # Null probes, invocation-exception fallbacks, successful results, and
    # contradictory or malformed identities never receive a fresh-absence
    # interpretation.
    $identitySucceededValue = Get-StartupObservationProperty $IdentityProbe 'Succeeded'
    $identityValue = Get-StartupObservationProperty $IdentityProbe 'Identity'
    $identityErrorValue = Get-StartupObservationProperty $IdentityProbe 'ErrorCode'
    $identityError = Convert-StartupObservationErrorCode $identityErrorValue
    $identityOperation = [string](Get-StartupObservationProperty $IdentityProbe 'Operation' 'exception')
    if ($startupIdentityOperations -notcontains $identityOperation) { $identityOperation = 'exception' }
    $identityProperty = if ($null -ne $IdentityProbe) { $IdentityProbe.PSObject.Properties['Identity'] } else { $null }
    if ($null -eq $IdentityProbe -or
        $identitySucceededValue -isnot [bool] -or [bool]$identitySucceededValue -or
        $null -eq $identityProperty -or $null -ne $identityValue -or
        $null -eq $identityError -or $identityError -le 0) {
        if ($RecordTrackedSweepTelemetry -and $null -ne $CleanupObservation) {
            Set-StartupTrackedSweepFailure $CleanupObservation 'identity-unavailable' 13
        }
        throw $ErrorMessage
    }
    $freshProbe = $null
    try {
        # This is deliberately one fresh typed census.  A complete, validated
        # census proving the exact PID is absent is the only benign race: the
        # process exited between the original snapshot and identity open.
        $freshProbe = if ($null -ne $FreshCensus) { & $FreshCensus } else { [NativeStartupProbe]::GetProcessEntries() }
    }
    catch {
        $freshProbe = [pscustomobject][ordered]@{
            Attempted = $true; AttemptCount = 1; Complete = $false; Succeeded = $false; ErrorCode = 13; RetryCount = 0; Retried = $false; Entries = @()
        }
    }
    if ($null -eq $freshProbe) {
        $freshProbe = [pscustomobject][ordered]@{
            Attempted = $true; AttemptCount = 1; Complete = $false; Succeeded = $false; ErrorCode = 13; RetryCount = 0; Retried = $false; Entries = @()
        }
    }
    if ($null -ne $CleanupObservation) { Add-StartupProcessEnumerationObservation $CleanupObservation $freshProbe }
    $failureType = Get-StartupTrackedSweepFailureType $ProcessId $freshProbe
    $freshError = Convert-StartupObservationErrorCode (Get-StartupObservationProperty $freshProbe 'ErrorCode')
    $failureError = if ($null -ne $identityError -and $identityError -gt 0) {
        [int]$identityError
    }
    elseif ($null -ne $freshError -and $freshError -gt 0) {
        [int]$freshError
    }
    else { 13 }
    switch ($failureType) {
        'identity-disappeared' {
            # Keep the original identity error as bounded diagnostic evidence
            # for the paired consumer, while allowing this sweep to continue
            # because the typed fresh census proved exit.  This also applies
            # when the caller did not request an observation object.
            if ($RecordTrackedSweepTelemetry -and $null -ne $CleanupObservation) {
                Set-StartupTrackedSweepFailure $CleanupObservation $failureType $failureError
                Add-StartupTrackedSweepCount $CleanupObservation 'trackedSweepDisappearedAfterSnapshotCount'
            }
            if ($ObserverRole -eq 'post-close-tracked-history') {
                if (-not (Set-StartupIdentityReconciliation $ContainmentProof $identityOperation $ObserverRole)) {
                    throw $ErrorMessage
                }
            }
            return $true
        }
        'identity-still-present' {
            if ($RecordTrackedSweepTelemetry -and $null -ne $CleanupObservation) {
                Set-StartupTrackedSweepFailure $CleanupObservation $failureType $failureError
                Add-StartupTrackedSweepCount $CleanupObservation 'trackedSweepStillPresentAfterFailureCount'
            }
        }
        default {
            if ($RecordTrackedSweepTelemetry -and $null -ne $CleanupObservation) {
                Set-StartupTrackedSweepFailure $CleanupObservation $failureType $failureError
            }
        }
    }
    throw $ErrorMessage
}

function Add-StartupCleanupError {
    param(
        [Parameter(Mandatory = $true)] [object]$Errors,
        [Parameter(Mandatory = $true)] [object]$Observation,
        [AllowNull()] [object]$Message
    )
    [void]$Errors.Add([string]$Message)
    try {
        $count = Convert-StartupObservationInt (Get-StartupObservationProperty $Observation 'cleanupErrorCount')
        if ($count -lt 0) { $count = 0 }
        if ($count -lt [int]::MaxValue) { $Observation.cleanupErrorCount = $count + 1 }
    }
    catch { }
}

function Try-StartupGracefulJobIdentityFallback {
    param(
        [AllowNull()] [object]$CleanupObservation,
        [AllowNull()] [object]$StillPresentBefore
    )
    if ($null -eq $CleanupObservation -or $StillPresentBefore -isnot [int32] -or
        $StillPresentBefore -lt 0 -or $StillPresentBefore -ge $startupObservationMaxCount) {
        return $false
    }
    $jobIdentityObservation = Get-StartupObservationProperty $CleanupObservation 'jobIdentityObservation'
    $stillPresentAfter = Get-StartupObservationProperty $jobIdentityObservation 'stillPresentAfterFailureCount'
    $fallbackType = Get-StartupObservationProperty $CleanupObservation 'gracefulCloseFallbackType'
    if ($stillPresentAfter -isnot [int32] -or
        $stillPresentAfter -ne ($StillPresentBefore + 1) -or
        [string]$fallbackType -ne 'none') {
        return $false
    }
    $CleanupObservation.gracefulCloseFallbackType = 'identity-still-present'
    return $true
}

function Get-StartupCleanupErrorCount {
    param(
        [AllowNull()] [object]$CleanupObservation,
        [AllowNull()] [object]$OuterErrorCount = 0,
        [bool]$JoinedCleanupErrorIncluded = $false
    )
    $internal = [decimal]0
    try { $internal = [decimal](Get-StartupObservationProperty $CleanupObservation 'cleanupErrorCount') } catch { $internal = [decimal]0 }
    if ($internal -lt 0) { $internal = [decimal]0 }
    if ($internal -gt [decimal][int]::MaxValue) { $internal = [decimal][int]::MaxValue }
    $outer = [decimal]0
    try { $outer = [decimal]$OuterErrorCount } catch { $outer = [decimal]0 }
    if ($outer -lt 0) { $outer = [decimal]0 }
    if ($JoinedCleanupErrorIncluded -and $outer -gt 0) { $outer-- }
    $total = $internal + $outer
    if ($total -gt [decimal][int]::MaxValue) { return [int]::MaxValue }
    return [int]$total
}

function Convert-StartupJobQueryObservation([AllowNull()] [object]$Query) {
    $observation = New-StartupJobQueryObservation
    if ($null -eq $Query) { return $observation }
    try {
        $observation.attempted = Convert-StartupObservationBool (Get-StartupObservationProperty $Query 'Attempted')
        $observation.queryCount = if ($observation.attempted) { 1 } else { 0 }
        $observation.succeeded = Convert-StartupObservationBool (Get-StartupObservationProperty $Query 'Succeeded')
        $errorValue = Get-StartupObservationProperty $Query 'ErrorCode'
        $observation.errorCode = if ($null -ne $errorValue) { Convert-StartupObservationInt $errorValue } else { $null }
        $observation.attemptCount = [Math]::Max(0, (Convert-StartupObservationInt (Get-StartupObservationProperty $Query 'AttemptCount')))
        $observation.capacityBytes = Convert-StartupObservationUInt64 (Get-StartupObservationProperty $Query 'CapacityBytes')
        $observation.requiredBytes = Convert-StartupObservationUInt64 (Get-StartupObservationProperty $Query 'RequiredBytes')
        $observation.returnLengthBytes = Convert-StartupObservationUInt64 (Get-StartupObservationProperty $Query 'ReturnLengthBytes')
        $observation.assignedProcessCount = Convert-StartupObservationUInt64 (Get-StartupObservationProperty $Query 'AssignedProcessCount')
        $observation.listedProcessCount = Convert-StartupObservationUInt64 (Get-StartupObservationProperty $Query 'ListedProcessCount')
        $observation.resized = Convert-StartupObservationBool (Get-StartupObservationProperty $Query 'Resized')
        $rawAttempts = @()
        $rawAttemptValue = Get-StartupObservationProperty $Query 'Attempts'
        if ($null -ne $rawAttemptValue) { $rawAttempts = @($rawAttemptValue) }
        $observation.attemptsTruncated = $rawAttempts.Count -gt 8
        $boundedAttempts = New-Object Collections.Generic.List[object]
        $attemptLimit = [Math]::Min($rawAttempts.Count, 8)
        for ($index = 0; $index -lt $attemptLimit; $index++) {
            $attempt = $rawAttempts[$index]
            if ($null -eq $attempt) { continue }
            $attemptError = Get-StartupObservationProperty $attempt 'ErrorCode' 0
            [void]$boundedAttempts.Add([ordered]@{
                attempted = Convert-StartupObservationBool (Get-StartupObservationProperty $attempt 'Attempted')
                attemptNumber = [Math]::Max(0, (Convert-StartupObservationInt (Get-StartupObservationProperty $attempt 'AttemptNumber')))
                succeeded = Convert-StartupObservationBool (Get-StartupObservationProperty $attempt 'Succeeded')
                errorCode = Convert-StartupObservationInt $attemptError
                capacityBytes = Convert-StartupObservationUInt64 (Get-StartupObservationProperty $attempt 'CapacityBytes')
                requiredBytes = Convert-StartupObservationUInt64 (Get-StartupObservationProperty $attempt 'RequiredBytes')
                returnLengthBytes = Convert-StartupObservationUInt64 (Get-StartupObservationProperty $attempt 'ReturnLengthBytes')
                assignedProcessCount = Convert-StartupObservationUInt64 (Get-StartupObservationProperty $attempt 'AssignedProcessCount')
                listedProcessCount = Convert-StartupObservationUInt64 (Get-StartupObservationProperty $attempt 'ListedProcessCount')
                resized = Convert-StartupObservationBool (Get-StartupObservationProperty $attempt 'Resized')
            })
        }
        $observation.attempts = $boundedAttempts.ToArray()
    }
    catch { return (New-StartupJobQueryObservation) }
    return $observation
}

function Add-StartupJobQueryObservation {
    param(
        [Parameter(Mandatory = $true)] [object]$Target,
        [AllowNull()] [object]$Observation
    )
    if ($null -eq $Observation) { return }
    try {
        $normalizedObservation = Convert-StartupJobQueryObservation $Observation
        $summaryTarget = $false
        if ($Target -is [Collections.IDictionary]) { $summaryTarget = $Target.Contains('query') }
        else { $summaryTarget = $null -ne $Target.PSObject.Properties['query'] }
        if ($summaryTarget) {
            $aggregate = Get-StartupObservationProperty $Target 'query'
            if ($null -eq $aggregate) {
                $aggregate = New-StartupJobQueryObservation
                $Target.query = $aggregate
            }
        }
        else {
            $aggregate = $Target
        }
        $aggregate.queryCount = (Convert-StartupObservationInt (Get-StartupObservationProperty $aggregate 'queryCount')) + 1
        $observationAttempted = Convert-StartupObservationBool (Get-StartupObservationProperty $normalizedObservation 'attempted')
        if ($observationAttempted) {
            if ($summaryTarget) {
                $Target.jobQueryAttempted = $true
                $Target.jobQuerySkipped = $false
            }
            $aggregate.attempted = $true
            $aggregate.skipped = $false
            $aggregate.succeeded = Convert-StartupObservationBool (Get-StartupObservationProperty $normalizedObservation 'succeeded')
            $aggregate.errorCode = Get-StartupObservationProperty $normalizedObservation 'errorCode'
            $aggregate.capacityBytes = Convert-StartupObservationUInt64 (Get-StartupObservationProperty $normalizedObservation 'capacityBytes')
            $aggregate.requiredBytes = Convert-StartupObservationUInt64 (Get-StartupObservationProperty $normalizedObservation 'requiredBytes')
            $aggregate.returnLengthBytes = Convert-StartupObservationUInt64 (Get-StartupObservationProperty $normalizedObservation 'returnLengthBytes')
            $aggregate.assignedProcessCount = Convert-StartupObservationUInt64 (Get-StartupObservationProperty $normalizedObservation 'assignedProcessCount')
            $aggregate.listedProcessCount = Convert-StartupObservationUInt64 (Get-StartupObservationProperty $normalizedObservation 'listedProcessCount')
            $aggregate.resized = (Convert-StartupObservationBool (Get-StartupObservationProperty $aggregate 'resized')) -or
                (Convert-StartupObservationBool (Get-StartupObservationProperty $normalizedObservation 'resized'))
        }
        elseif ((Convert-StartupObservationBool (Get-StartupObservationProperty $normalizedObservation 'skipped')) -and
            -not (Convert-StartupObservationBool (Get-StartupObservationProperty $aggregate 'attempted'))) {
            $aggregate.skipped = $true
            if ($summaryTarget) { $Target.jobQuerySkipped = $true }
        }
        $aggregate.attemptCount = (Convert-StartupObservationInt (Get-StartupObservationProperty $aggregate 'attemptCount')) +
            (Convert-StartupObservationInt (Get-StartupObservationProperty $normalizedObservation 'attemptCount'))
        $aggregate.attemptsTruncated = (Convert-StartupObservationBool (Get-StartupObservationProperty $aggregate 'attemptsTruncated')) -or
            (Convert-StartupObservationBool (Get-StartupObservationProperty $normalizedObservation 'attemptsTruncated'))
        $boundedAttempts = New-Object Collections.Generic.List[object]
        $aggregateAttempts = Get-StartupObservationProperty $aggregate 'attempts'
        $observationAttempts = Get-StartupObservationProperty $normalizedObservation 'attempts'
        foreach ($attempt in @($aggregateAttempts) + @($observationAttempts)) {
            if ($boundedAttempts.Count -ge 8) {
                $aggregate.attemptsTruncated = $true
                break
            }
            if ($null -ne $attempt) { [void]$boundedAttempts.Add($attempt) }
        }
        $aggregate.attempts = $boundedAttempts.ToArray()
    }
    catch { }
}

function New-StartupDiagnosticState {
    param([int]$RootProcessId = 0)
    $snapshots = New-Object Collections.Generic.List[object]
    foreach ($checkpoint in @($startupDiagnosticCheckpointNames)) {
        [void]$snapshots.Add([ordered]@{
            checkpoint = $checkpoint
            targetMs = [int]$startupDiagnosticCheckpointMs[$checkpoint]
            status = 'not-reached'
            observed = $false
            elapsedMs = $null
            processTree = @()
            processCount = 0
            processRecordsTruncated = $false
            jobMembershipVerified = $false
            jobMemberCount = 0
            jobQueryObservation = New-StartupJobQueryObservation
            topLevelWindowCount = $null
            topLevelWindowCountCapped = $false
            editorWindowCount = $null
            dialogWindowCount = $null
            otherWindowCount = $null
            rootExitState = 'not-observed'
            rootExitCode = $null
            rootExitErrorCode = $null
            processExitObserved = $false
            processExitElapsedMs = $null
            failureStage = $null
            failureType = $null
            failureSubstage = 'none'
            failureErrorCode = $null
        })
    }
    return [ordered]@{
        schemaVersion = 1
        rootProcessId = [int]$RootProcessId
        processTreeSnapshots = $snapshots.ToArray()
        processExitObservation = [ordered]@{
            observed = $false
            elapsedMs = $null
            pid = if ($RootProcessId -gt 0) { [int]$RootProcessId } else { $null }
            source = 'run-root'
            state = 'not-observed'
            exitCode = $null
            errorCode = $null
        }
    }
}

function Convert-StartupDiagnosticElapsedMs([AllowNull()] [object]$Value) {
    if ($null -eq $Value) { return $null }
    $elapsed = 0.0
    try { $elapsed = [double]$Value } catch { return $null }
    if ([double]::IsNaN($elapsed) -or [double]::IsInfinity($elapsed) -or $elapsed -lt 0.0) { return $null }
    return [double]([Math]::Round([Math]::Min($elapsed, [double]$startupTimeoutMs), 3))
}

function Get-StartupDiagnosticImageName($Record) {
    $imageName = $null
    try { $imageName = [IO.Path]::GetFileName([string]$Record.ImagePath) } catch { $imageName = $null }
    if ([string]::IsNullOrWhiteSpace($imageName) -or $imageName.Length -gt $startupDiagnosticMaxImageNameLength -or
        $imageName.IndexOfAny([char[]]@('\', '/', ':')) -ge 0 -or $imageName -match '[\r\n]') {
        return 'unavailable'
    }
    return [string]$imageName
}

function Convert-StartupDiagnosticProcessMetadata($Record, [bool]$JobMember) {
    return [ordered]@{
        pid = [int]$Record.Id
        ppid = [int]$Record.ParentId
        imageName = Get-StartupDiagnosticImageName $Record
        creationTime = [long]$Record.Creation
        jobMember = [bool]$JobMember
    }
}

function Get-StartupDiagnosticJobMembers([IntPtr]$Job) {
    $members = @{}
    if ($Job -eq [IntPtr]::Zero) {
        $skipped = New-StartupJobQueryObservation
        $skipped.skipped = $true
        return [pscustomobject]@{ verified = $false; members = $members; queryObservation = $skipped }
    }
    $query = [NativeStartupProbe]::QueryJobProcessIds($Job)
    $queryObservation = Convert-StartupJobQueryObservation $query
    if (-not $query.Succeeded -or $null -eq $query.ProcessIds) {
        return [pscustomobject]@{ verified = $false; members = $members; queryObservation = $queryObservation }
    }
    foreach ($processId in @($query.ProcessIds)) {
        if ([int]$processId -gt 0) { $members[[int]$processId] = $true }
    }
    return [pscustomobject]@{ verified = $true; members = $members; queryObservation = $queryObservation }
}

function Get-StartupDiagnosticWindowCount {
    param(
        [Parameter(Mandatory = $true)] [object]$ProcessIds,
        [AllowNull()] [object[]]$Windows = $null
    )
    if ($null -eq $Windows) { $Windows = @([NativeStartupProbe]::GetTopLevelWindows()) }
    $count = 0
    $editorCount = 0
    $dialogCount = 0
    $otherCount = 0
    $capped = $false
    foreach ($window in @($Windows)) {
        if ($null -eq $ProcessIds -or -not $ProcessIds.ContainsKey([int]$window.ProcessId)) { continue }
        if ($count -ge $startupDiagnosticMaxWindowCount) {
            $capped = $true
            break
        }
        $className = [string]$window.ClassName
        if ($className.StartsWith('TextEditorWindow', [StringComparison]::Ordinal)) { $editorCount++ }
        elseif ($className.Equals('#32770', [StringComparison]::Ordinal)) { $dialogCount++ }
        else { $otherCount++ }
        $count++
    }
    return [pscustomobject][ordered]@{
        count = $count
        capped = $capped
        editorWindowCount = $editorCount
        dialogWindowCount = $dialogCount
        otherWindowCount = $otherCount
    }
}

function Add-StartupDiagnosticCheckpoint {
    param(
        [Parameter(Mandatory = $true)] [object]$State,
        [Parameter(Mandatory = $true)] [string]$Checkpoint,
        [Parameter(Mandatory = $true)] [object]$Owned,
        [Parameter(Mandatory = $true)] [IntPtr]$Job,
        [Parameter(Mandatory = $true)] [double]$ElapsedMs,
        [IntPtr]$RootProcessHandle = [IntPtr]::Zero,
        [switch]$Force,
        [string]$FailureStage = $null,
        [string]$FailureType = $null,
        [AllowNull()] [object]$Invokers = $null
    )
    if (-not $startupDiagnosticCheckpointNames.Contains($Checkpoint)) { return $false }
    $entry = @($State.processTreeSnapshots | Where-Object { $_.checkpoint -eq $Checkpoint } | Select-Object -First 1)
    if ($entry.Count -eq 0) { return $false }
    $snapshot = $entry[0]
    if ([bool]$snapshot.observed -and -not $Force) { return $false }

    $safeElapsedMs = Convert-StartupDiagnosticElapsedMs $ElapsedMs
    if ($null -eq $safeElapsedMs) { $safeElapsedMs = [double]$startupTimeoutMs }
    $jobInfo = [pscustomobject]@{ verified = $false; members = @{}; queryObservation = (New-StartupJobQueryObservation) }
    $jobMembers = @{}
    $processSnapshot = $null
    $exitProbe = $null
    $processObservation = New-StartupDiagnosticProcessObservation
    $diagnosticPhase = [ordered]@{ substage = 'root-exit'; errorCode = $null }
    $nonfatalSubstage = 'none'
    $nonfatalErrorCode = $null
    try {
        # Keep the handle returned by CreateProcessW until the checkpoint and
        # cleanup phases finish.  Querying this owned handle avoids PID reuse
        # and makes STILL_ACTIVE an explicit active state rather than an exit.
        $exitInvoker = Get-StartupObservationProperty $Invokers 'ExitProbe'
        $exitProbe = if ($exitInvoker -is [scriptblock]) {
            & $exitInvoker $RootProcessHandle
        }
        else {
            [NativeStartupProbe]::QueryProcessExitState($RootProcessHandle)
        }
        if (-not $exitProbe.Succeeded) {
            $exitErrorCode = Convert-StartupObservationErrorCode (Get-StartupObservationProperty $exitProbe 'ErrorCode')
            $exitFailureErrorCode = if ($null -eq $exitErrorCode -or $exitErrorCode -eq 0) { 13 } else { [int]$exitErrorCode }
            Set-StartupDiagnosticPhase $diagnosticPhase 'root-exit' $exitFailureErrorCode
            throw "Could not observe the run-root exit state (Win32 $($exitProbe.ErrorCode))."
        }
        Set-StartupDiagnosticPhase $diagnosticPhase 'job-membership'
        $jobInvoker = Get-StartupObservationProperty $Invokers 'JobMembers'
        $jobInfo = if ($jobInvoker -is [scriptblock]) {
            & $jobInvoker $Job
        }
        else {
            Get-StartupDiagnosticJobMembers $Job
        }
        $jobMembers = $jobInfo.members
        $snapshot.jobQueryObservation = $jobInfo.queryObservation
        if (-not [bool]$jobInfo.verified -and $Job -ne [IntPtr]::Zero) {
            $nonfatalSubstage = 'job-membership'
            $jobErrorCode = Convert-StartupObservationErrorCode (Get-StartupObservationProperty $jobInfo.queryObservation 'errorCode')
            $nonfatalErrorCode = if ($null -eq $jobErrorCode -or $jobErrorCode -eq 0) { 13 } else { [int]$jobErrorCode }
        }
        $snapshot.rootExitState = if ($exitProbe.Active) { 'active' } else { 'exited' }
        $snapshot.rootExitCode = [UInt64]$exitProbe.ExitCode
        $snapshot.rootExitErrorCode = $null
        $State.processExitObservation.state = $snapshot.rootExitState
        $State.processExitObservation.exitCode = [UInt64]$exitProbe.ExitCode
        $State.processExitObservation.errorCode = $null
        if (-not $exitProbe.Active -and -not [bool]$State.processExitObservation.observed) {
            $State.processExitObservation.observed = $true
            $State.processExitObservation.elapsedMs = $safeElapsedMs
        }
        Set-StartupDiagnosticPhase $diagnosticPhase 'process-census-first'
        $firstProcessInvoker = Get-StartupObservationProperty $Invokers 'ProcessSnapshotFirst'
        $identityInvoker = Get-StartupObservationProperty $Invokers 'ProcessIdentity'
        $processEntriesInvoker = Get-StartupObservationProperty $Invokers 'ProcessEntries'
        $processSnapshot = if ($firstProcessInvoker -is [scriptblock]) {
            & $firstProcessInvoker $Owned $diagnosticPhase
        }
        else {
            Get-ProcessSnapshot $Owned @() $processObservation $diagnosticPhase 'process-census-first' `
                $identityInvoker $processEntriesInvoker
        }
        Update-OwnedProcesses $Owned $processSnapshot
        Set-StartupDiagnosticPhase $diagnosticPhase 'process-census-second'
        $secondProcessInvoker = Get-StartupObservationProperty $Invokers 'ProcessSnapshotSecond'
        $processSnapshot = if ($secondProcessInvoker -is [scriptblock]) {
            & $secondProcessInvoker $Owned $diagnosticPhase
        }
        else {
            Get-ProcessSnapshot $Owned @() $processObservation $diagnosticPhase 'process-census-second' `
                $identityInvoker $processEntriesInvoker
        }
        Set-StartupDiagnosticPhase $diagnosticPhase 'projection-finalize'
        $records = @($Owned.Values | Where-Object { Test-ProcessIdentity $_ $processSnapshot } | Sort-Object Id)
        $processIds = @{}
        foreach ($record in $records) { $processIds[[int]$record.Id] = $true }
        Set-StartupDiagnosticPhase $diagnosticPhase 'window-enumeration'
        $windowInvoker = Get-StartupObservationProperty $Invokers 'WindowCount'
        $windowInfo = if ($windowInvoker -is [scriptblock]) {
            & $windowInvoker $processIds
        }
        else {
            Get-StartupDiagnosticWindowCount $processIds
        }
        Set-StartupDiagnosticPhase $diagnosticPhase 'projection-finalize'
        $projectionInvoker = Get-StartupObservationProperty $Invokers 'ProjectionFinalize'
        if ($projectionInvoker -is [scriptblock]) {
            & $projectionInvoker $records $windowInfo
        }
        $tree = New-Object Collections.Generic.List[object]
        $recordLimit = [Math]::Min($records.Count, $startupDiagnosticMaxProcessCount)
        for ($index = 0; $index -lt $recordLimit; $index++) {
            $record = $records[$index]
            $member = [bool]$false
            if ($jobInfo.verified) { $member = $jobMembers.ContainsKey([int]$record.Id) }
            [void]$tree.Add((Convert-StartupDiagnosticProcessMetadata $record $member))
        }
        $snapshot.processTree = $tree.ToArray()
        $snapshot.processCount = [int]$recordLimit
        $snapshot.processRecordsTruncated = [bool]($records.Count -gt $recordLimit)
        $snapshot.jobMembershipVerified = [bool]$jobInfo.verified
        $snapshot.jobMemberCount = if ($jobInfo.verified) { [int](@($records | Where-Object { $jobMembers.ContainsKey([int]$_.Id) }).Count) } else { 0 }
        $snapshot.topLevelWindowCount = [int]$windowInfo.count
        $snapshot.topLevelWindowCountCapped = [bool]$windowInfo.capped
        $snapshot.editorWindowCount = [int]$windowInfo.editorWindowCount
        $snapshot.dialogWindowCount = [int]$windowInfo.dialogWindowCount
        $snapshot.otherWindowCount = [int]$windowInfo.otherWindowCount
        $snapshot.processExitObserved = [bool]$State.processExitObservation.observed
        $snapshot.processExitElapsedMs = $State.processExitObservation.elapsedMs
        $snapshot.status = 'observed'
        $snapshot.observed = $true
        $snapshot.elapsedMs = $safeElapsedMs
        $snapshot.failureStage = $FailureStage
        $snapshot.failureType = $FailureType
        $snapshot.failureSubstage = if ($nonfatalSubstage -eq 'none') { 'none' } else { $nonfatalSubstage }
        $snapshot.failureErrorCode = if ($nonfatalSubstage -eq 'none') { $null } else { [int]$nonfatalErrorCode }
        return $true
    }
    catch {
        # Diagnostic collection is deliberately typed and payload-free.  The
        # launch result remains authoritative; cleanup still performs the
        # normal identity and Job checks below.
        $snapshot.status = 'unavailable'
        $snapshot.observed = $false
        $snapshot.elapsedMs = $safeElapsedMs
        $snapshot.processTree = @()
        $snapshot.processCount = 0
        $snapshot.processRecordsTruncated = $false
        $snapshot.jobMembershipVerified = [bool]$jobInfo.verified
        $snapshot.jobMemberCount = 0
        $snapshot.jobQueryObservation = $jobInfo.queryObservation
        $snapshot.topLevelWindowCount = $null
        $snapshot.topLevelWindowCountCapped = $false
        $snapshot.editorWindowCount = $null
        $snapshot.dialogWindowCount = $null
        $snapshot.otherWindowCount = $null
        $failureSubstage = [string](Get-StartupObservationProperty $diagnosticPhase 'substage')
        if (-not (Test-StartupDiagnosticFailureSubstage $failureSubstage)) { $failureSubstage = 'projection-finalize' }
        $failureErrorCode = Convert-StartupObservationErrorCode (Get-StartupObservationProperty $diagnosticPhase 'errorCode')
        if ($null -eq $failureErrorCode -or $failureErrorCode -eq 0) {
            $failureErrorCode = Convert-StartupObservationErrorCode (Get-StartupObservationProperty $processObservation 'processEnumerationErrorCode')
        }
        if ($null -eq $failureErrorCode -or $failureErrorCode -eq 0) {
            $failureErrorCode = 13
        }
        if ($null -eq $exitProbe -or -not $exitProbe.Succeeded) {
            $snapshot.rootExitState = 'unavailable'
            $snapshot.rootExitCode = $null
            $snapshot.rootExitErrorCode = if ($null -ne $exitProbe) {
                $exitErrorCode = Convert-StartupObservationErrorCode (Get-StartupObservationProperty $exitProbe 'ErrorCode')
                if ($null -eq $exitErrorCode) { [int]$failureErrorCode } else { [int]$exitErrorCode }
            }
            else { [int]$failureErrorCode }
            $State.processExitObservation.state = 'unavailable'
            $State.processExitObservation.exitCode = $null
            $State.processExitObservation.errorCode = $snapshot.rootExitErrorCode
        }
        $snapshot.processExitObserved = [bool]$State.processExitObservation.observed
        $snapshot.processExitElapsedMs = $State.processExitObservation.elapsedMs
        $snapshot.failureStage = 'process-tree'
        $snapshot.failureType = 'observation'
        $snapshot.failureSubstage = $failureSubstage
        $snapshot.failureErrorCode = [int]$failureErrorCode
        return $false
    }
}

function Update-StartupDiagnosticCheckpoints {
    param(
        [Parameter(Mandatory = $true)] [object]$State,
        [Parameter(Mandatory = $true)] [object]$Owned,
        [Parameter(Mandatory = $true)] [IntPtr]$Job,
        [Parameter(Mandatory = $true)] [double]$ElapsedMs,
        [IntPtr]$RootProcessHandle = [IntPtr]::Zero
    )
    foreach ($checkpoint in @('0.5s', '2s', '10s')) {
        $entry = @($State.processTreeSnapshots | Where-Object { $_.checkpoint -eq $checkpoint } | Select-Object -First 1)
        if ($entry.Count -eq 0 -or [string]$entry[0].status -ne 'not-reached') { continue }
        if ($ElapsedMs -lt [double]$startupDiagnosticCheckpointMs[$checkpoint]) { continue }
        [void](Add-StartupDiagnosticCheckpoint $State $checkpoint $Owned $Job $ElapsedMs -RootProcessHandle $RootProcessHandle)
    }
}

function Get-VerifiedProcessEntries {
    param(
        [AllowNull()] [object]$Observation = $null,
        [AllowNull()] [scriptblock]$Invoker = $null
    )
    try {
        $probe = if ($null -ne $Invoker) { & $Invoker } else { [NativeStartupProbe]::GetProcessEntries() }
    }
    catch {
        if ($null -ne $Observation) {
            $probe = [pscustomobject][ordered]@{
                Attempted = $true; Complete = $false; Succeeded = $false
                AttemptCount = 1; ErrorCode = 13; RetryCount = 0; Entries = @()
            }
            Add-StartupProcessEnumerationObservation $Observation $probe
        }
        throw
    }
    if ($null -eq $probe) {
        # A null native result is a failed typed census with no payload. Record
        # it before the common validation path so fail-closed callers cannot
        # lose the attempt from cleanup telemetry.
        $probe = [pscustomobject][ordered]@{
            Attempted = $true; Complete = $false; Succeeded = $false
            AttemptCount = 1; ErrorCode = 13; RetryCount = 0; Entries = @()
        }
    }
    if ($null -ne $Observation) { Add-StartupProcessEnumerationObservation $Observation $probe }

    # Treat the native result as an untrusted envelope even though the default
    # invoker is local C#.  A forged/string/partial result must never become a
    # process-tree or identity decision through PowerShell coercion.
    $attemptedValue = Get-StartupObservationProperty $probe 'Attempted'
    $completeValue = Get-StartupObservationProperty $probe 'Complete'
    $succeededValue = Get-StartupObservationProperty $probe 'Succeeded'
    $errorValue = Get-StartupObservationProperty $probe 'ErrorCode'
    # Read Entries through the raw property so a one-member array is not
    # collapsed to its scalar element by PowerShell pipeline unrolling.
    $entriesValue = $null
    if ($null -ne $probe) {
        if ($probe -is [Collections.IDictionary]) {
            if ($probe.Contains('Entries')) { $entriesValue = $probe['Entries'] }
        }
        else {
            $entriesProperty = $probe.PSObject.Properties['Entries']
            if ($null -ne $entriesProperty) { $entriesValue = $entriesProperty.Value }
        }
    }
    $attemptsValue = Get-StartupObservationProperty $probe 'AttemptCount'
    $retryValue = Get-StartupObservationProperty $probe 'RetryCount'
    $retriedValue = Get-StartupObservationProperty $probe 'Retried'
    $attemptCountValid = Test-StartupIntegralValue $attemptsValue 1 ([decimal]$startupProcessEnumerationMaxAttemptCount)
    $retryCountValid = Test-StartupIntegralValue $retryValue 0 ([decimal]($startupProcessEnumerationMaxAttemptCount - 1))
    $envelopeValid = $null -ne $probe -and
        $attemptedValue -is [bool] -and [bool]$attemptedValue -and
        $completeValue -is [bool] -and $succeededValue -is [bool] -and
        (Test-StartupIntegralValue $errorValue 0 ([decimal][int]::MaxValue)) -and
        $attemptCountValid -and $retryCountValid -and $retriedValue -is [bool] -and
        $entriesValue -is [Array] -and $entriesValue.Length -le $startupProcessEnumerationMaxEntryCount
    if ($envelopeValid) {
        # Get-VerifiedProcessEntries is a success-only trust boundary.  A
        # coherent typed failure is recorded above but must still throw; its
        # partial/empty Entries payload is never returned as a snapshot.
        # In native-shaped terms, -not [bool]$probe.Complete or -not [bool]$probe.Succeeded is therefore always fail-closed here.
        $errorCode = [int]$errorValue
        $attemptCount = [int]$attemptsValue
        $retryCount = [int]$retryValue
        $coherentEnvelope = $attemptCount -ge 1 -and $attemptCount -le $startupProcessEnumerationMaxAttemptCount -and
            $retryCount -ge 0 -and $retryCount -lt $attemptCount -and
            [bool]$retriedValue -eq ($retryCount -gt 0) -and
            (([bool]$succeededValue -and [bool]$completeValue -and $errorCode -eq 0) -or
             (-not [bool]$succeededValue -and -not [bool]$completeValue -and $errorCode -gt 0))
        # A coherent native failure is useful for telemetry, but this resolver
        # returns entries only for a successful census.  Never expose the
        # failure's (possibly empty) payload as a snapshot.
        $envelopeValid = $coherentEnvelope -and [bool]$succeededValue
    }
    $entryIds = @{}
    if ($envelopeValid) {
        foreach ($entry in $entriesValue) {
            if ($null -eq $entry -or $entry -is [string] -or $entry -is [ValueType] -or $entry -is [Array]) {
                $envelopeValid = $false
                break
            }
            $processIdValue = Get-StartupObservationProperty $entry 'ProcessId'
            $parentProcessIdValue = Get-StartupObservationProperty $entry 'ParentProcessId'
            $imageNameValue = Get-StartupObservationProperty $entry 'ImageName'
            if (-not (Test-StartupIntegralValue $processIdValue 0 ([decimal][int]::MaxValue)) -or
                -not (Test-StartupIntegralValue $parentProcessIdValue 0 ([decimal][int]::MaxValue)) -or
                $imageNameValue -isnot [string] -or [string]::IsNullOrWhiteSpace($imageNameValue) -or
                $imageNameValue.Length -ge $startupDiagnosticMaxImageNameLength) {
                $envelopeValid = $false
                break
            }
            $processId = [int]$processIdValue
            if ($entryIds.ContainsKey($processId)) {
                $envelopeValid = $false
                break
            }
            $entryIds[$processId] = $true
        }
    }
    if (-not $envelopeValid) {
        $errorCode = if (Test-StartupIntegralValue $errorValue 1 ([decimal][int]::MaxValue)) { [int]$errorValue } else { 13 }
        $attemptCount = if (Test-StartupIntegralValue $attemptsValue 0 ([decimal][int]::MaxValue)) { [int]$attemptsValue } else { 0 }
        $retryCount = if (Test-StartupIntegralValue $retryValue 0 ([decimal][int]::MaxValue)) { [int]$retryValue } else { 0 }
        throw "Could not enumerate the native process snapshot (Win32 $errorCode; attempts $attemptCount; retries $retryCount)."
    }
    return @($entriesValue)
}

function Resolve-StartupProcessIdentity {
    param(
        [Parameter(Mandatory = $true)] [object]$Entry,
        [AllowNull()] [object]$IdentityProbe,
        [AllowNull()] [object]$Observation,
        [AllowNull()] [object]$DiagnosticPhase,
        [Parameter(Mandatory = $true)] [string]$DiagnosticCensusSubstage,
        [AllowNull()] [string]$IdentitySubstage,
        [AllowNull()] [scriptblock]$FreshEntriesInvoker = $null
    )
    $identitySucceeded = Test-StartupIdentitySuccessEnvelope $IdentityProbe
    $identity = Get-StartupObservationProperty $IdentityProbe 'Identity'
    $entryProcessId = [int](Get-StartupObservationProperty $Entry 'ProcessId')
    $entryParentProcessId = [int](Get-StartupObservationProperty $Entry 'ParentProcessId')
    if ($identitySucceeded) {
        # A successful native envelope is still untrusted at this boundary:
        # validate its complete identity and the exact census PID/PPID before
        # allowing it to become an owned process record.
        if (Test-StartupJobIdentityShape $identity $entryProcessId $entryParentProcessId) {
            try {
                return Convert-ProcessIdentity $identity
            }
            catch {
                if ($null -ne $DiagnosticPhase) {
                    Set-StartupDiagnosticPhase $DiagnosticPhase $IdentitySubstage 13
                }
                throw "Could not convert the identity of relevant process $entryProcessId."
            }
        }
        $identityErrorCode = Convert-StartupObservationErrorCode (Get-StartupObservationProperty $IdentityProbe 'ErrorCode')
        if ($null -eq $identityErrorCode -or $identityErrorCode -eq 0) { $identityErrorCode = 13 }
        if ($null -ne $DiagnosticPhase) {
            Set-StartupDiagnosticPhase $DiagnosticPhase $IdentitySubstage $identityErrorCode
        }
        throw "Could not validate the identity of relevant process $entryProcessId."
    }
    # A malformed or contradictory envelope is not a coherent disappearance.
    # Do not use a fresh census to turn invalid success data into acceptance.
    if (-not (Test-StartupCoherentIdentityFailure $IdentityProbe)) {
        $identityErrorCode = Convert-StartupObservationErrorCode (Get-StartupObservationProperty $IdentityProbe 'ErrorCode')
        if ($null -eq $identityErrorCode -or $identityErrorCode -eq 0) { $identityErrorCode = 13 }
        if ($null -ne $DiagnosticPhase) {
            Set-StartupDiagnosticPhase $DiagnosticPhase $IdentitySubstage $identityErrorCode
        }
        throw "Could not verify the identity of relevant process $entryProcessId."
    }
    $identityErrorCode = Convert-StartupObservationErrorCode (Get-StartupObservationProperty $IdentityProbe 'ErrorCode')
    if ($null -eq $identityErrorCode -or $identityErrorCode -eq 0) { $identityErrorCode = 13 }
    # The identity failure is retained only while checking whether the entry is
    # still present. A fresh census transition clears it so a benign
    # disappearance cannot contaminate the next census/window phase.
    if ($null -ne $DiagnosticPhase) { Set-StartupDiagnosticPhase $DiagnosticPhase $DiagnosticCensusSubstage }
    $freshEntries = @(Get-VerifiedProcessEntries $Observation $FreshEntriesInvoker)
    $stillPresent = @($freshEntries | Where-Object {
        [int](Get-StartupObservationProperty $_ 'ProcessId') -eq [int](Get-StartupObservationProperty $Entry 'ProcessId')
    }).Count -gt 0
    if ($stillPresent) {
        if ($null -ne $DiagnosticPhase) {
            Set-StartupDiagnosticPhase $DiagnosticPhase $IdentitySubstage $identityErrorCode
        }
        throw "Could not verify the identity of relevant process $((Get-StartupObservationProperty $Entry 'ProcessId')) (Win32 $($IdentityProbe.ErrorCode))."
    }
    return $null
}

function Get-ProcessSnapshot(
    $Owned,
    [int[]]$SeedIds = @(),
    [object]$Observation = $null,
    [AllowNull()] [object]$DiagnosticPhase = $null,
    [string]$DiagnosticCensusSubstage = 'process-census-first',
    [AllowNull()] [scriptblock]$IdentityInvoker = $null,
    [AllowNull()] [scriptblock]$ProcessEntriesInvoker = $null
) {
    $identitySubstage = if ([StringComparer]::Ordinal.Equals($DiagnosticCensusSubstage, 'process-census-first')) {
        'process-identity-first'
    }
    elseif ([StringComparer]::Ordinal.Equals($DiagnosticCensusSubstage, 'process-census-second')) {
        'process-identity-second'
    }
    else { $null }
    if ($null -ne $DiagnosticPhase) { Set-StartupDiagnosticPhase $DiagnosticPhase $DiagnosticCensusSubstage }
    $entries = @(Get-VerifiedProcessEntries $Observation $ProcessEntriesInvoker)
    $relevant = @{}
    if ($null -ne $Owned) {
        foreach ($id in @($Owned.Keys)) { $relevant[[int]$id] = $true }
    }
    foreach ($id in @($SeedIds)) { $relevant[[int]$id] = $true }

    $changed = $true
    while ($changed) {
        $changed = $false
        foreach ($entry in $entries) {
            if ($relevant.ContainsKey([int]$entry.ProcessId)) { continue }
            if (-not $relevant.ContainsKey([int]$entry.ParentProcessId)) { continue }
            $relevant[[int]$entry.ProcessId] = $true
            $changed = $true
        }
    }

    $result = @{}
    foreach ($entry in $entries) {
        if (-not $relevant.ContainsKey([int]$entry.ProcessId)) { continue }
        if ($null -ne $DiagnosticPhase -and $null -ne $identitySubstage) {
            Set-StartupDiagnosticPhase $DiagnosticPhase $identitySubstage
        }
        $identityProbe = if ($null -ne $IdentityInvoker) {
            & $IdentityInvoker $entry
        }
        else {
            [NativeStartupProbe]::QueryProcessIdentity([int]$entry.ProcessId, [int]$entry.ParentProcessId)
        }
        $record = Resolve-StartupProcessIdentity $entry $identityProbe $Observation $DiagnosticPhase `
            $DiagnosticCensusSubstage $identitySubstage $ProcessEntriesInvoker
        if ($null -eq $record) { continue }
        $result[$record.Id] = $record
    }

    # The Toolhelp PID/PPID closure above is intentionally only a candidate
    # set.  PPIDs can be reused, so a candidate must also have a temporally
    # possible edge to a verified parent before it is exposed as a snapshot.
    # Keep already-owned roots usable when their parent is no longer present;
    # their identity is still the caller's prior ownership record and the
    # descendant edge is checked against that record below.
    $rootIds = @{}
    if ($null -ne $Owned) {
        foreach ($id in @($Owned.Keys)) { $rootIds[[int]$id] = $true }
    }
    foreach ($id in @($SeedIds)) { $rootIds[[int]$id] = $true }
    $accepted = @{}
    foreach ($id in @($rootIds.Keys)) {
        $numericId = [int]$id
        if ($result.ContainsKey($numericId)) { $accepted[$numericId] = $result[$numericId] }
    }

    $changed = $true
    while ($changed) {
        $changed = $false
        foreach ($record in @($result.Values)) {
            $recordId = [int]$record.Id
            if ($accepted.ContainsKey($recordId)) { continue }
            $parentId = [int]$record.ParentId
            $parent = $null
            if ($accepted.ContainsKey($parentId)) {
                $parent = $accepted[$parentId]
            }
            elseif (-not $result.ContainsKey($parentId) -and $null -ne $Owned -and $Owned.ContainsKey($parentId)) {
                $parent = $Owned[$parentId]
            }
            # A parent present in the current candidate set but not accepted
            # yet must be handled by a later pass; a parent rejected for an
            # impossible edge must never be used as an alternate root.
            if ($null -eq $parent) { continue }
            if (-not (Test-StartupDescendantCreationOrder $parent $record)) { continue }
            $accepted[$recordId] = $record
            $changed = $true
        }
    }
    return $accepted
}

function Get-ProcessesForImagePath([string]$ImagePath, [object]$Observation = $null) {
    $imageName = [IO.Path]::GetFileName($ImagePath)
    $result = @()
    foreach ($entry in @(Get-VerifiedProcessEntries $Observation)) {
        if (-not [string]::Equals([string]$entry.ImageName, $imageName, [StringComparison]::OrdinalIgnoreCase)) { continue }
        $identityProbe = $null
        try {
            $identityProbe = [NativeStartupProbe]::QueryProcessIdentity([int]$entry.ProcessId, [int]$entry.ParentProcessId)
        }
        catch {
            $proof = Get-StartupObservationProperty $Observation 'containmentProof'
            [void](Set-StartupIdentityReconciliation $proof 'exception' 'exact-path')
            throw
        }
        if (-not $identityProbe.Succeeded -or $null -eq $identityProbe.Identity) {
            # Exact-path observation is a mandatory independent gate.  Neither a
            # later PID absence nor the Job proof may reconcile its identity
            # failure, because this observer is the only exact-image-path check.
            $operation = [string](Get-StartupObservationProperty $identityProbe 'Operation' 'exception')
            $proof = Get-StartupObservationProperty $Observation 'containmentProof'
            [void](Set-StartupIdentityReconciliation $proof $operation 'exact-path')
            throw "Could not verify the identity of matching process $($entry.ProcessId) (Win32 $($identityProbe.ErrorCode))."
        }
        $record = Convert-ProcessIdentity $identityProbe.Identity
        if (Test-SamePath $record.ImagePath $ImagePath) { $result += $record }
    }
    return $result
}

function Test-StartupCoherentIdentityFailure {
    param([AllowNull()] [object]$IdentityProbe)
    $identitySucceededValue = Get-StartupObservationProperty $IdentityProbe 'Succeeded'
    $identityValue = Get-StartupObservationProperty $IdentityProbe 'Identity'
    $identityErrorValue = Get-StartupObservationProperty $IdentityProbe 'ErrorCode'
    $identityError = if (Test-StartupIntegralValue $identityErrorValue 1 ([decimal][int]::MaxValue)) { [int]$identityErrorValue } else { $null }
    $identityProperty = if ($null -ne $IdentityProbe) {
        if ($IdentityProbe -is [Collections.IDictionary]) { $IdentityProbe.Contains('Identity') }
        else { $null -ne $IdentityProbe.PSObject.Properties['Identity'] }
    }
    return $null -ne $IdentityProbe -and
        $identitySucceededValue -is [bool] -and -not [bool]$identitySucceededValue -and
        $identityProperty -and $null -eq $identityValue -and
        $null -ne $identityError -and $identityError -gt 0
}

function Test-StartupIdentitySuccessEnvelope {
    param([AllowNull()] [object]$IdentityProbe)
    if ($null -eq $IdentityProbe) { return $false }
    $identityProperty = if ($IdentityProbe -is [Collections.IDictionary]) {
        $IdentityProbe.Contains('Identity')
    }
    else {
        $null -ne $IdentityProbe.PSObject.Properties['Identity']
    }
    $succeededValue = Get-StartupObservationProperty $IdentityProbe 'Succeeded'
    $errorValue = Get-StartupObservationProperty $IdentityProbe 'ErrorCode'
    $identityValue = Get-StartupObservationProperty $IdentityProbe 'Identity'
    return $succeededValue -is [bool] -and [bool]$succeededValue -and
        (Test-StartupIntegralValue $errorValue 0 ([decimal][int]::MaxValue)) -and
        [int]$errorValue -eq 0 -and $identityProperty -and $null -ne $identityValue
}

function Test-StartupJobIdentityShape {
    param(
        [AllowNull()] [object]$Identity,
        [Parameter(Mandatory = $true)] [int]$RequestedProcessId,
        [Parameter(Mandatory = $true)] [int]$ExpectedParentProcessId
    )
    if ($null -eq $Identity -or $Identity -is [string] -or $Identity -is [ValueType] -or $Identity -is [Array]) { return $false }
    $processIdValue = Get-StartupObservationProperty $Identity 'ProcessId'
    $parentProcessIdValue = Get-StartupObservationProperty $Identity 'ParentProcessId'
    $creationTimeValue = Get-StartupObservationProperty $Identity 'CreationTime'
    $imagePathValue = Get-StartupObservationProperty $Identity 'ImagePath'
    $intMaximum = [int]::MaxValue
    $longMaximum = [long]::MaxValue
    if (-not (Test-StartupIntegralValue $processIdValue 1 ([decimal]$intMaximum)) -or
        -not (Test-StartupIntegralValue $parentProcessIdValue 0 ([decimal]$intMaximum)) -or
        -not (Test-StartupIntegralValue $creationTimeValue 1 ([decimal]$longMaximum)) -or
        $imagePathValue -isnot [string] -or [string]::IsNullOrWhiteSpace($imagePathValue) -or
        $imagePathValue.Length -ge 32768) {
        return $false
    }
    try {
        return [int]$processIdValue -eq $RequestedProcessId -and
            [int]$parentProcessIdValue -eq $ExpectedParentProcessId
    }
    catch { return $false }
}

function Test-StartupStrictJobQueryEnvelope {
    param([AllowNull()] [object]$Candidate)
    $attemptedValue = Get-StartupObservationProperty $Candidate 'Attempted'
    $succeededValue = Get-StartupObservationProperty $Candidate 'Succeeded'
    # Read the raw PID-array property directly so a one-member Job result
    # is not collapsed to a scalar by PowerShell pipeline unrolling.
    $processIdsValue = $null
    if ($null -ne $Candidate) {
        if ($Candidate -is [Collections.IDictionary]) {
            if ($Candidate.Contains('ProcessIds')) { $processIdsValue = $Candidate['ProcessIds'] }
        }
        else {
            $processIdsProperty = $Candidate.PSObject.Properties['ProcessIds']
            if ($null -ne $processIdsProperty) { $processIdsValue = $processIdsProperty.Value }
        }
    }
    # Attempts is another raw array boundary.  Do not let PowerShell unwrap
    # a one-attempt array into a scalar before validating its native shape.
    $attemptsValue = $null
    if ($null -ne $Candidate) {
        if ($Candidate -is [Collections.IDictionary]) {
            if ($Candidate.Contains('Attempts')) { $attemptsValue = $Candidate['Attempts'] }
        }
        else {
            $attemptsProperty = $Candidate.PSObject.Properties['Attempts']
            if ($null -ne $attemptsProperty) { $attemptsValue = $attemptsProperty.Value }
        }
    }
    $errorRaw = Get-StartupObservationProperty $Candidate 'ErrorCode'
    $attemptCountRaw = Get-StartupObservationProperty $Candidate 'AttemptCount'
    $capacityRaw = Get-StartupObservationProperty $Candidate 'CapacityBytes'
    $requiredRaw = Get-StartupObservationProperty $Candidate 'RequiredBytes'
    $returnLengthRaw = Get-StartupObservationProperty $Candidate 'ReturnLengthBytes'
    $assignedRaw = Get-StartupObservationProperty $Candidate 'AssignedProcessCount'
    $listedRaw = Get-StartupObservationProperty $Candidate 'ListedProcessCount'
    $resizedValue = Get-StartupObservationProperty $Candidate 'Resized'
    # These types mirror StartupProbeJobResult exactly.  In particular,
    # accepting a mathematically integral Double or Decimal here would let
    # a forged envelope cross the Job-membership decision boundary.
    $errorValid = $errorRaw -is [int32] -and $errorRaw -ge 0 -and $errorRaw -le [int32]::MaxValue
    $attemptCountValid = $attemptCountRaw -is [int32] -and
        $attemptCountRaw -ge 1 -and $attemptCountRaw -le $startupJobQueryMaxAttemptCount
    $byteFieldsValid = $capacityRaw -is [UInt64] -and $capacityRaw -le $startupJobQueryMaxBytes -and
        $requiredRaw -is [UInt64] -and $requiredRaw -le $startupJobQueryMaxBytes -and
        $returnLengthRaw -is [UInt64] -and $returnLengthRaw -le $startupJobQueryMaxBytes
    $countFieldsValid = $assignedRaw -is [UInt32] -and $assignedRaw -le [UInt32]$startupJobQueryMaxProcessCount -and
        $listedRaw -is [UInt32] -and $listedRaw -le [UInt32]$startupJobQueryMaxProcessCount
    $errorCode = if ($errorValid) { [int]$errorRaw } else { 13 }
    $queryShapeValid = $null -ne $Candidate -and
        $attemptedValue -is [bool] -and [bool]$attemptedValue -and
        $succeededValue -is [bool] -and $resizedValue -is [bool] -and $errorValid -and
        $attemptCountValid -and $byteFieldsValid -and $countFieldsValid -and
        $null -ne $processIdsValue -and $processIdsValue -is [Array] -and
        $processIdsValue.Length -le $startupJobQueryMaxProcessCount -and
        $null -ne $attemptsValue -and $attemptsValue -is [Array] -and
        $attemptsValue.Length -le $startupJobQueryMaxAttemptCount
    if (-not $queryShapeValid) {
        return [pscustomobject][ordered]@{ valid = $false; processIds = [int[]]@(); errorCode = $errorCode; reason = 'top-level-shape' }
    }
    $attemptCount = [int]$attemptCountRaw
    $capacityBytes = [UInt64]$capacityRaw
    $requiredBytes = [UInt64]$requiredRaw
    $returnLengthBytes = [UInt64]$returnLengthRaw
    $assignedProcessCount = [UInt32]$assignedRaw
    $listedProcessCount = [UInt32]$listedRaw
    if ($attemptsValue.Length -ne $attemptCount) {
        return [pscustomobject][ordered]@{ valid = $false; processIds = [int[]]@(); errorCode = 13; reason = 'attempt-count-equation' }
    }
    $attemptsValid = $true
    $anyAttemptResized = $false
    $previousDataCapacity = $null
    $lastAttempt = $null
    for ($attemptIndex = 0; $attemptIndex -lt $attemptsValue.Length; $attemptIndex++) {
        $attempt = $attemptsValue[$attemptIndex]
        if ($null -eq $attempt -or $attempt -is [string] -or $attempt -is [ValueType] -or $attempt -is [Array]) {
            $attemptsValid = $false
            break
        }
        $attemptAttempted = Get-StartupObservationProperty $attempt 'Attempted'
        $attemptNumber = Get-StartupObservationProperty $attempt 'AttemptNumber'
        $attemptSucceeded = Get-StartupObservationProperty $attempt 'Succeeded'
        $attemptError = Get-StartupObservationProperty $attempt 'ErrorCode'
        $attemptCapacity = Get-StartupObservationProperty $attempt 'CapacityBytes'
        $attemptRequired = Get-StartupObservationProperty $attempt 'RequiredBytes'
        $attemptReturnLength = Get-StartupObservationProperty $attempt 'ReturnLengthBytes'
        $attemptAssigned = Get-StartupObservationProperty $attempt 'AssignedProcessCount'
        $attemptListed = Get-StartupObservationProperty $attempt 'ListedProcessCount'
        $attemptResized = Get-StartupObservationProperty $attempt 'Resized'
        $attemptValid = $attemptAttempted -is [bool] -and [bool]$attemptAttempted -and
            $attemptNumber -is [int32] -and $attemptNumber -eq ($attemptIndex + 1) -and
            $attemptSucceeded -is [bool] -and $attemptError -is [int32] -and
            $attemptError -ge 0 -and $attemptError -le [int32]::MaxValue -and
            $attemptCapacity -is [UInt64] -and $attemptCapacity -le $startupJobQueryMaxBytes -and
            $attemptRequired -is [UInt64] -and $attemptRequired -le $startupJobQueryMaxBytes -and
            $attemptReturnLength -is [UInt64] -and $attemptReturnLength -le $startupJobQueryMaxBytes -and
            $attemptAssigned -is [UInt32] -and $attemptAssigned -le [UInt32]$startupJobQueryMaxProcessCount -and
            $attemptListed -is [UInt32] -and $attemptListed -le [UInt32]$startupJobQueryMaxProcessCount -and
            $attemptResized -is [bool] -and $attemptListed -le $attemptAssigned
        if ($attemptValid) {
            $attemptCapacity = [UInt64]$attemptCapacity
            $attemptRequired = [UInt64]$attemptRequired
            $attemptReturnLength = [UInt64]$attemptReturnLength
            $attemptAssigned = [UInt32]$attemptAssigned
            $attemptListed = [UInt32]$attemptListed
            if ($attemptCapacity -eq [UInt64]0) {
                # The zero-buffer sizing query is the only native attempt
                # allowed to have zero capacity and zero counts.  Its
                # required and returned lengths are the same sizing hint.
                $attemptValid = $attemptIndex -eq 0 -and $attemptAssigned -eq 0 -and
                    $attemptListed -eq 0 -and $attemptRequired -eq $attemptReturnLength -and
                    -not [bool]$attemptResized
            }
            else {
                $minimumCapacity = $startupJobQueryHeaderBytes + [UInt64]([IntPtr]::Size)
                $capacitySlots = ($attemptCapacity - $startupJobQueryHeaderBytes) / [UInt64]([IntPtr]::Size)
                $attemptValid = $attemptCapacity -ge $minimumCapacity -and
                    $attemptRequired -eq $attemptCapacity -and
                    [UInt64]$attemptListed -le $capacitySlots -and
                    ($null -eq $previousDataCapacity -or $attemptCapacity -gt [UInt64]$previousDataCapacity)
            }
            if ($attemptValid -and [bool]$attemptSucceeded) {
                $attemptValid = $attemptError -eq 0
                if ($attemptValid) {
                    if ($attemptReturnLength -eq [UInt64]0) {
                        $attemptValid = $attemptAssigned -eq 0 -and $attemptListed -eq 0
                    }
                    else {
                        $minimumReturn = $startupJobQueryHeaderBytes +
                            ([UInt64]$attemptListed * [UInt64]([IntPtr]::Size))
                        $attemptValid = $attemptReturnLength -ge $minimumReturn -and
                            $attemptReturnLength -le $attemptCapacity
                    }
                }
            }
            elseif ($attemptValid) {
                # A typed native failure must carry a nonzero Win32 code.
                # Retryable failures may report a return length larger than
                # the current buffer; the core uses that value to grow it.
                $attemptValid = $attemptError -gt 0
            }
        }
        if ($attemptValid -and $attemptCapacity -gt [UInt64]0) {
            $previousDataCapacity = [UInt64]$attemptCapacity
        }
        if ($attemptValid) {
            $isFinalAttempt = $attemptIndex -eq ($attemptsValue.Length - 1)
            $isRetryableFailure = -not [bool]$attemptSucceeded -and
                $startupJobQueryRetryableErrorCodes -contains [int]$attemptError
            if (-not $isFinalAttempt) {
                if ($attemptCapacity -eq [UInt64]0) {
                    # The first zero-buffer sizing attempt may precede a
                    # data query, but a terminal non-retryable sizing
                    # failure cannot have a later attempt.
                    $attemptValid = $attemptIndex -eq 0 -and
                        ([bool]$attemptSucceeded -or $isRetryableFailure)
                }
                elseif ([bool]$attemptSucceeded) {
                    # A successful non-final attempt is legal only when it
                    # was a partial list that the native loop resized.
                    $attemptValid = $attemptListed -lt $attemptAssigned -and [bool]$attemptResized
                }
                else {
                    # A non-final data failure must be retryable and must
                    # carry the native loop's resize marker.
                    $attemptValid = $isRetryableFailure -and [bool]$attemptResized
                }
            }
            elseif ([bool]$attemptSucceeded -and [bool]$attemptResized) {
                # A successful result is complete; a resize marker on the
                # final attempt is contradictory.  (A failed exhausted
                # attempt may retain its final resize hint.)
                $attemptValid = $false
            }
        }
        if (-not $attemptValid) {
            $attemptsValid = $false
            break
        }
        if ([bool]$attemptResized) { $anyAttemptResized = $true }
        $lastAttempt = $attempt
    }
    if (-not $attemptsValid -or $null -eq $lastAttempt) {
        return [pscustomobject][ordered]@{ valid = $false; processIds = [int[]]@(); errorCode = 13; reason = 'attempt-equation' }
    }
    $lastCapacity = [UInt64](Get-StartupObservationProperty $lastAttempt 'CapacityBytes')
    $lastRequired = [UInt64](Get-StartupObservationProperty $lastAttempt 'RequiredBytes')
    $lastReturnLength = [UInt64](Get-StartupObservationProperty $lastAttempt 'ReturnLengthBytes')
    $lastAssigned = [UInt32](Get-StartupObservationProperty $lastAttempt 'AssignedProcessCount')
    $lastListed = [UInt32](Get-StartupObservationProperty $lastAttempt 'ListedProcessCount')
    $lastSucceeded = [bool](Get-StartupObservationProperty $lastAttempt 'Succeeded')
    $lastError = [int](Get-StartupObservationProperty $lastAttempt 'ErrorCode')
    $lastResized = [bool](Get-StartupObservationProperty $lastAttempt 'Resized')
    $requiredMatchesLastAttempt = $requiredBytes -eq $lastRequired
    # QueryJobProcessIdsCore records the next growth target when a
    # retryable final attempt exhausts the eight-call budget.  Preserve
    # that native diagnostic while still requiring a bounded, advancing
    # target; all other results must match the final attempt exactly.
    if (-not $requiredMatchesLastAttempt -and $lastResized -and
        $attemptCount -eq $startupJobQueryMaxAttemptCount -and
        $requiredBytes -gt $lastRequired) {
        $requiredMatchesLastAttempt = $requiredBytes -le $startupJobQueryMaxBytes
    }
    $queryShapeValid = $capacityBytes -eq $lastCapacity -and
        $requiredMatchesLastAttempt -and $returnLengthBytes -eq $lastReturnLength -and
        $assignedProcessCount -eq $lastAssigned -and $listedProcessCount -eq $lastListed -and
        [bool]$resizedValue -eq $anyAttemptResized
    if (-not $queryShapeValid) {
        return [pscustomobject][ordered]@{
            valid = $false; processIds = [int[]]@(); errorCode = if ($errorCode -gt 0) { $errorCode } else { 13 }
            reason = 'top-level-final-equation'
        }
    }
    if ([bool]$succeededValue) {
        if ($queryShapeValid) {
            $queryShapeValid = $errorCode -eq 0 -and $lastSucceeded -and $lastError -eq 0 -and
                -not $lastResized -and $assignedProcessCount -eq $listedProcessCount -and
                $processIdsValue.Length -eq [int]$listedProcessCount
            if ($queryShapeValid) {
                if ($returnLengthBytes -eq [UInt64]0) {
                    $queryShapeValid = $assignedProcessCount -eq 0 -and $listedProcessCount -eq 0
                }
                else {
                    $minimumReturn = $startupJobQueryHeaderBytes +
                        ([UInt64]$listedProcessCount * [UInt64]([IntPtr]::Size))
                    $queryShapeValid = $capacityBytes -ge ($startupJobQueryHeaderBytes + [UInt64]([IntPtr]::Size)) -and
                        $returnLengthBytes -ge $minimumReturn -and $returnLengthBytes -le $capacityBytes
                }
            }
        }
    }
    else {
        # Failure envelopes never publish process IDs and must retain their
        # first nonzero native error; a false success/error-zero pair is
        # contradictory even if all metadata fields look well-typed.
        $queryShapeValid = $queryShapeValid -and $errorCode -gt 0 -and $processIdsValue.Length -eq 0
    }
    $ids = New-Object Collections.Generic.List[int]
    $seen = @{}
    if (-not $queryShapeValid -or -not [bool]$succeededValue) {
        return [pscustomobject][ordered]@{
            valid = $false; processIds = [int[]]@(); errorCode = if ($errorCode -gt 0) { $errorCode } else { 13 }
            reason = if ([bool]$succeededValue) { 'terminal-status-equation' } else { 'query-failed' }
        }
    }
    foreach ($rawProcessId in $processIdsValue) {
        if ($null -eq $rawProcessId -or $rawProcessId -is [bool]) {
            return [pscustomobject][ordered]@{ valid = $false; processIds = [int[]]@(); errorCode = 13; reason = 'process-id-type' }
        }
        if (-not (Test-StartupIntegralValue $rawProcessId 1 ([decimal][int]::MaxValue))) {
            return [pscustomobject][ordered]@{ valid = $false; processIds = [int[]]@(); errorCode = 13; reason = 'process-id-range' }
        }
        try { $processId = [int]$rawProcessId } catch {
            return [pscustomobject][ordered]@{ valid = $false; processIds = [int[]]@(); errorCode = 13; reason = 'process-id-range' }
        }
        if ($seen.ContainsKey($processId)) {
            return [pscustomobject][ordered]@{ valid = $false; processIds = [int[]]@(); errorCode = 13; reason = 'process-id-duplicate' }
        }
        $seen[$processId] = $true
        [void]$ids.Add($processId)
    }
    return [pscustomobject][ordered]@{ valid = $true; processIds = $ids.ToArray(); errorCode = 0; reason = 'none' }
}

function Get-JobProcessRecords([IntPtr]$Job, $Owned, [object]$QueryObservation = $null, [object]$CleanupObservation = $null, [AllowNull()] [object]$Invokers = $null, [AllowNull()] [object]$JobIdentityObservation = $null, [string]$ObserverRole = 'launch-job-member') {
    if ($Job -eq [IntPtr]::Zero) { throw 'A run-owned job handle is required.' }
    if ($startupIdentityObserverRoles -notcontains $ObserverRole -or $ObserverRole -eq 'none') {
        throw 'The Job identity observer role is not allowlisted.'
    }

    # Keep the seam local to this production path.  Normal measurement calls
    # use the native query/census/identity invokers; the no-GUI self-test can
    # inject deterministic results without creating a process or a GUI.
    $jobQueryInvoker = { param([IntPtr]$Handle) [NativeStartupProbe]::QueryJobProcessIds($Handle) }
    $processCensusInvoker = { [NativeStartupProbe]::GetProcessEntries() }
    $identityQueryInvoker = { param([int]$ProcessId, [int]$ParentProcessId) [NativeStartupProbe]::QueryProcessIdentity($ProcessId, $ParentProcessId) }
    if ($null -ne $Invokers) {
        $candidateJobQueryInvoker = Get-StartupObservationProperty $Invokers 'JobQuery'
        $candidateProcessCensusInvoker = Get-StartupObservationProperty $Invokers 'ProcessCensus'
        $candidateIdentityQueryInvoker = Get-StartupObservationProperty $Invokers 'IdentityQuery'
        if ($candidateJobQueryInvoker -isnot [scriptblock] -or
            $candidateProcessCensusInvoker -isnot [scriptblock] -or
            $candidateIdentityQueryInvoker -isnot [scriptblock]) {
            throw 'The run-owned Job resolver/invoker seam is malformed.'
        }
        $jobQueryInvoker = $candidateJobQueryInvoker
        $processCensusInvoker = $candidateProcessCensusInvoker
        $identityQueryInvoker = $candidateIdentityQueryInvoker
    }
    $jobIdentityObservation = if ($null -ne $JobIdentityObservation) {
        $JobIdentityObservation
    }
    elseif ($null -ne $CleanupObservation) {
        Get-StartupObservationProperty $CleanupObservation 'jobIdentityObservation'
    }
    else { $null }
    $newJobQueryFailure = {
        return [pscustomobject][ordered]@{
            Attempted = $true; Succeeded = $false; ErrorCode = 13; ProcessIds = [int[]]@()
            AttemptCount = 1; CapacityBytes = [UInt64]0; RequiredBytes = [UInt64]0; ReturnLengthBytes = [UInt64]0
            AssignedProcessCount = [UInt32]0; ListedProcessCount = [UInt32]0; Resized = $false
            Attempts = @([pscustomobject][ordered]@{
                Attempted = $true; AttemptNumber = 1; Succeeded = $false; ErrorCode = 13
                CapacityBytes = [UInt64]0; RequiredBytes = [UInt64]0; ReturnLengthBytes = [UInt64]0
                AssignedProcessCount = [UInt32]0; ListedProcessCount = [UInt32]0; Resized = $false
            })
        }
    }
    $validateJobQuery = { param([AllowNull()] [object]$Candidate) Test-StartupStrictJobQueryEnvelope $Candidate }
    $query = $null
    try {
        $query = & $jobQueryInvoker $Job
    }
    catch {
        if ($null -ne $QueryObservation) { Add-StartupJobQueryObservation $QueryObservation (& $newJobQueryFailure) }
        throw
    }
    $queryMetadata = Convert-StartupJobQueryObservation $query
    if ($null -ne $QueryObservation) { Add-StartupJobQueryObservation $QueryObservation $queryMetadata }
    $queryCheck = & $validateJobQuery $query
    if (-not $queryCheck.valid) {
        throw "Could not enumerate run-owned job processes (Win32 $($queryCheck.errorCode))."
    }
    $entries = @(Get-VerifiedProcessEntries -Observation $CleanupObservation -Invoker $processCensusInvoker)
    $records = New-Object Collections.Generic.List[object]
    foreach ($processId in @($queryCheck.processIds)) {
        $entry = @($entries | Where-Object { [int]$_.ProcessId -eq [int]$processId } | Select-Object -First 1)
        if ($entry.Count -eq 0) {
            # The process may have exited after the job query.  Re-query the job
            # once before declaring an identity gap; an unobservable member is not
            # a clean result.
            $retry = $null
            try {
                $retry = & $jobQueryInvoker $Job
            }
            catch {
                if ($null -ne $QueryObservation) { Add-StartupJobQueryObservation $QueryObservation (& $newJobQueryFailure) }
                throw
            }
            $retryMetadata = Convert-StartupJobQueryObservation $retry
            if ($null -ne $QueryObservation) { Add-StartupJobQueryObservation $QueryObservation $retryMetadata }
            $retryCheck = & $validateJobQuery $retry
            if (-not $retryCheck.valid -or @($retryCheck.processIds | Where-Object { [int]$_ -eq [int]$processId }).Count -gt 0) {
                throw "Could not observe the identity of run-owned job process $processId."
            }
            continue
        }
        if ($null -ne $jobIdentityObservation) { Add-StartupJobIdentityCount $jobIdentityObservation 'identityAttemptCount' }
        $identityProbe = $null
        try {
            $identityProbe = & $identityQueryInvoker ([int]$entry[0].ProcessId) ([int]$entry[0].ParentProcessId)
        }
        catch {
            if ($null -ne $jobIdentityObservation) {
                Add-StartupJobIdentityCount $jobIdentityObservation 'identityFailureCount'
                Set-StartupJobIdentityFailure $jobIdentityObservation 'exception' 13 'exception' $ObserverRole
            }
            throw "Could not verify the identity of run-owned job process $processId (Win32 13)."
        }
        $identitySucceeded = Test-StartupIdentitySuccessEnvelope $identityProbe
        $identityFailedCoherently = Test-StartupCoherentIdentityFailure $identityProbe
        $identityOperation = [string](Get-StartupObservationProperty $identityProbe 'Operation' 'exception')
        if ($startupIdentityOperations -notcontains $identityOperation) { $identityOperation = 'exception' }
        if (-not $identitySucceeded -and -not $identityFailedCoherently) {
            $identityError = Convert-StartupObservationErrorCode (Get-StartupObservationProperty $identityProbe 'ErrorCode')
            if ($null -eq $identityError -or $identityError -le 0) { $identityError = 13 }
            if ($null -ne $jobIdentityObservation) {
                Add-StartupJobIdentityCount $jobIdentityObservation 'identityFailureCount'
                Set-StartupJobIdentityFailure $jobIdentityObservation 'identity-unavailable' $identityError $identityOperation $ObserverRole
            }
            throw "Could not verify the identity of run-owned job process $processId (Win32 $identityError)."
        }
        if (-not $identitySucceeded) {
            $identityError = Convert-StartupObservationErrorCode (Get-StartupObservationProperty $identityProbe 'ErrorCode')
            if ($null -eq $identityError -or $identityError -le 0) { $identityError = 13 }
            if ($null -ne $jobIdentityObservation) {
                Add-StartupJobIdentityCount $jobIdentityObservation 'identityFailureCount'
                Add-StartupJobIdentityCount $jobIdentityObservation 'recoveryAttemptCount'
            }

            # Job membership identity gaps use a fresh Job-membership query to
            # prove that the exact
            # member is gone.  A Toolhelp census alone cannot distinguish a stale
            # membership result from an exited member.
            $freshJobQuery = $null
            try {
                $freshJobQuery = & $jobQueryInvoker $Job
            }
            catch {
                if ($null -ne $QueryObservation) { Add-StartupJobQueryObservation $QueryObservation (& $newJobQueryFailure) }
                if ($null -ne $jobIdentityObservation) { Set-StartupJobIdentityFailure $jobIdentityObservation 'exception' 13 'exception' $ObserverRole }
                throw "Could not refresh run-owned job membership after identity failure for process $processId (Win32 13)."
            }
            $freshJobMetadata = Convert-StartupJobQueryObservation $freshJobQuery
            if ($null -ne $QueryObservation) { Add-StartupJobQueryObservation $QueryObservation $freshJobMetadata }
            $freshJobCheck = & $validateJobQuery $freshJobQuery
            if (-not $freshJobCheck.valid) {
                if ($null -ne $jobIdentityObservation) { Set-StartupJobIdentityFailure $jobIdentityObservation 'enumeration-unavailable' $freshJobCheck.errorCode $identityOperation $ObserverRole }
                throw "Could not refresh run-owned job membership after identity failure for process $processId (Win32 $($freshJobCheck.errorCode))."
            }
            if (@($freshJobCheck.processIds | Where-Object { [int]$_ -eq [int]$processId }).Count -gt 0) {
                if ($null -ne $jobIdentityObservation) {
                    Add-StartupJobIdentityCount $jobIdentityObservation 'stillPresentAfterFailureCount'
                    Set-StartupJobIdentityFailure $jobIdentityObservation 'identity-still-present' $identityError $identityOperation $ObserverRole
                }
                throw "Could not verify the identity of run-owned job process $processId (Win32 $identityError)."
            }

            # The fresh Job-membership query is the sole recovery proof.  Do not
            # add a second Toolhelp census here: between two snapshots a PID may
            # be reused, and a second census would reintroduce that ambiguity.
            if ($null -ne $jobIdentityObservation) {
                Add-StartupJobIdentityCount $jobIdentityObservation 'disappearedAfterSnapshotCount'
                Set-StartupJobIdentityFailure $jobIdentityObservation 'identity-disappeared' $identityError $identityOperation $ObserverRole
            }
            continue
        }
        # The conversion is kept below the recovery branch so a successful
        # identity probe still fails closed if conversion itself is malformed.
        if (-not (Test-StartupJobIdentityShape $identityProbe.Identity ([int]$entry[0].ProcessId) ([int]$entry[0].ParentProcessId))) {
            if ($null -ne $jobIdentityObservation) {
                Add-StartupJobIdentityCount $jobIdentityObservation 'identityFailureCount'
                Set-StartupJobIdentityFailure $jobIdentityObservation 'exception' 13 'exception' $ObserverRole
            }
            throw "Could not validate the identity of run-owned job process $processId (Win32 13)."
        }
        try {
            $record = Convert-ProcessIdentity $identityProbe.Identity
        }
        catch {
            if ($null -ne $jobIdentityObservation) {
                Add-StartupJobIdentityCount $jobIdentityObservation 'identityFailureCount'
                Set-StartupJobIdentityFailure $jobIdentityObservation 'exception' 13 'exception' $ObserverRole
            }
            throw "Could not convert the identity of run-owned job process $processId (Win32 13)."
        }
        $Owned[$record.Id] = $record
        [void]$records.Add($record)
    }
    return $records.ToArray()
}

function Test-ProcessIdentity($Record, $Snapshot) {
    if (-not $Snapshot.ContainsKey([int]$Record.Id)) { return $false }
    $current = $Snapshot[[int]$Record.Id]
    return $current.Creation -eq $Record.Creation -and (Test-SamePath $current.ImagePath $Record.ImagePath)
}

function Test-StartupDescendantCreationOrder {
    param(
        [AllowNull()] [object]$Parent,
        [AllowNull()] [object]$Child
    )
    if ($null -eq $Parent -or $null -eq $Child) { return $false }
    $parentIdValue = Get-StartupObservationProperty $Parent 'Id'
    $childIdValue = Get-StartupObservationProperty $Child 'Id'
    $childParentIdValue = Get-StartupObservationProperty $Child 'ParentId'
    $parentCreationValue = Get-StartupObservationProperty $Parent 'Creation'
    $childCreationValue = Get-StartupObservationProperty $Child 'Creation'
    $intMaximum = [decimal][int]::MaxValue
    $longMaximum = [decimal][long]::MaxValue
    if (-not (Test-StartupIntegralValue $parentIdValue 1 $intMaximum) -or
        -not (Test-StartupIntegralValue $childIdValue 1 $intMaximum) -or
        -not (Test-StartupIntegralValue $childParentIdValue 1 $intMaximum) -or
        -not (Test-StartupIntegralValue $parentCreationValue 1 $longMaximum) -or
        -not (Test-StartupIntegralValue $childCreationValue 1 $longMaximum)) {
        return $false
    }
    try {
        if ([int]$childParentIdValue -ne [int]$parentIdValue) { return $false }
        # FILETIME observations can share a tick when the underlying clock is
        # coarser than the 100 ns representation.  Equality is therefore
        # temporally possible; only a child strictly older than its parent is
        # impossible and is rejected as a stale PID/PPID link.
        return [long]$childCreationValue -ge [long]$parentCreationValue
    }
    catch { return $false }
}

function Update-OwnedProcesses($Owned, $Snapshot) {
    $changed = $true
    while ($changed) {
        $changed = $false
        foreach ($candidate in @($Snapshot.Values)) {
            if ($Owned.ContainsKey($candidate.Id)) { continue }
            if (-not $Owned.ContainsKey($candidate.ParentId)) { continue }
            $parent = $Owned[$candidate.ParentId]
            if (-not (Test-ProcessIdentity $parent $Snapshot)) { continue }
            if (-not (Test-StartupDescendantCreationOrder $parent $candidate)) { continue }
            $Owned[$candidate.Id] = $candidate
            $changed = $true
        }
    }
}

function Get-LiveOwnedProcesses($Owned, [IntPtr]$Job = [IntPtr]::Zero, [object]$QueryObservation = $null, [object]$CleanupObservation = $null, [string]$ObserverRole = 'graceful-job-member') {
    if ($Job -ne [IntPtr]::Zero) {
        return @(Get-JobProcessRecords $Job $Owned $QueryObservation $CleanupObservation $null $null $ObserverRole)
    }
    $snapshot = Get-ProcessSnapshot $Owned @() $CleanupObservation
    Update-OwnedProcesses $Owned $snapshot
    return @($Owned.Values | Where-Object { Test-ProcessIdentity $_ $snapshot })
}

function Wait-WithPoll([scriptblock]$Condition, [int]$TimeoutMs) {
    # Do not call this variable $watch: condition scriptblocks use PowerShell's
    # dynamic scope and may intentionally read the caller's end-to-end stopwatch.
    # Shadowing it here makes milestone timestamps non-monotonic.
    $pollWatch = [Diagnostics.Stopwatch]::StartNew()
    while ($pollWatch.ElapsedMilliseconds -lt $TimeoutMs) {
        if (& $Condition) { return $true }
        Start-Sleep -Milliseconds $pollIntervalMs
    }
    return (& $Condition)
}

function Update-StartupReadinessState($State, [double]$ObservedMs, [bool]$CaptionReady, [bool]$InputIdleReady, [int]$ScrollMaximum, [int]$ExpectedLineCount) {
    # Each readiness condition is sampled by the same polling pass, but retains the
    # timestamp from the first pass on which that specific condition is observed.
    # This keeps one late condition from inflating the others.
    if ($CaptionReady -and $null -eq $State.captionReadyMs) {
        $State.captionReadyMs = $ObservedMs
    }
    if ($InputIdleReady -and -not $State.inputIdleReached) {
        $State.inputIdleReached = $true
        $State.inputIdleMs = $ObservedMs
    }
    if ($ScrollMaximum -gt $State.verticalScrollMaximum) {
        $State.verticalScrollMaximum = $ScrollMaximum
    }
    if ($ScrollMaximum -ge $ExpectedLineCount -and $null -eq $State.documentReadyMs) {
        $State.documentReadyMs = $ObservedMs
    }
    # WaitForInputIdle is a process-level, one-time proxy that any GUI thread can
    # satisfy.  Retain it as a diagnostic, but do not let an intermittent miss
    # veto the externally observed caption and complete document layout.
    return $null -ne $State.captionReadyMs -and $null -ne $State.documentReadyMs
}

function Get-StartupReadinessMissingMilestones($State, [int]$ExpectedLineCount) {
    $missing = New-Object Collections.Generic.List[string]
    if ($null -eq $State.captionReadyMs) { $missing.Add('visible document caption') }
    if ($null -eq $State.documentReadyMs) {
        $missing.Add(('document layout (scrollbar maximum {0}; expected at least {1})' -f $State.verticalScrollMaximum, $ExpectedLineCount))
    }
    return @($missing.ToArray())
}

function Get-UpperPercentile([double[]]$Values, [double]$Percentile) {
    if ($Values.Count -eq 0 -or $Percentile -lt 0.0 -or $Percentile -gt 100.0) {
        throw 'Percentile input is invalid.'
    }
    $ordered = @($Values | Sort-Object)
    $rank = [int][Math]::Ceiling(($Percentile / 100.0) * $ordered.Count)
    if ($rank -lt 1) { $rank = 1 }
    if ($rank -gt $ordered.Count) { $rank = $ordered.Count }
    return [double]$ordered[$rank - 1]
}

function Get-Statistics([double[]]$Values) {
    $ordered = @($Values | Sort-Object)
    if ($ordered.Count -eq 0) { return $null }
    $middle = [int][Math]::Floor($ordered.Count / 2)
    if (($ordered.Count % 2) -eq 1) { $median = [double]$ordered[$middle] }
    else { $median = ([double]$ordered[$middle - 1] + [double]$ordered[$middle]) / 2.0 }
    return [ordered]@{
        count = $ordered.Count
        medianMs = $median
        p95Ms = Get-UpperPercentile ([double[]]$ordered) 95.0
        minMs = [double]$ordered[0]
        maxMs = [double]$ordered[$ordered.Count - 1]
        meanMs = [Math]::Round((($ordered | Measure-Object -Average).Average), 3)
    }
}

function Get-Sha256([string]$Path) {
    $algorithm = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::OpenRead($Path)
    try {
        return ([BitConverter]::ToString($algorithm.ComputeHash($stream))).Replace('-', '').ToLowerInvariant()
    }
    finally {
        $stream.Dispose()
        $algorithm.Dispose()
    }
}

# Sakura resolves legacy settings from the executable-side sakura.exe.ini
# before it initializes shared data.  A missing sidecar falls back to the
# current user's roaming profile, so every measurement launch must use an
# owned copied executable with this exact portable-mode contract.
$script:StartupProfileSidecarContract = 'sakura.exe.ini|Settings|MultiUser=0|utf16le-bom-crlf-v1'
$script:StartupProfileSidecarFileName = 'sakura.exe.ini'
$script:StartupProfileSidecarText = "[Settings]`r`nMultiUser=0`r`n"

function Get-StartupProfileSidecarBytes {
    $encoding = [Text.UnicodeEncoding]::new($false, $true)
    $body = $encoding.GetBytes($script:StartupProfileSidecarText)
    $preamble = $encoding.GetPreamble()
    $bytes = New-Object byte[] ($preamble.Length + $body.Length)
    [Array]::Copy($preamble, 0, $bytes, 0, $preamble.Length)
    [Array]::Copy($body, 0, $bytes, $preamble.Length, $body.Length)
    return $bytes
}

function Get-ByteArraySha256([byte[]]$Bytes) {
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try {
        return ([BitConverter]::ToString($algorithm.ComputeHash($Bytes))).Replace('-', '').ToLowerInvariant()
    }
    finally { $algorithm.Dispose() }
}

function Get-StartupProfileSidecarContractIdentity {
    $bytes = Get-StartupProfileSidecarBytes
    return [pscustomobject][ordered]@{
        contract = $script:StartupProfileSidecarContract
        fileName = $script:StartupProfileSidecarFileName
        sha256 = Get-ByteArraySha256 $bytes
        sizeBytes = [UInt64]$bytes.Length
        multiUser = 0
    }
}

function Get-StartupProfileSidecarPath([string]$ExecutablePath) {
    if ([string]::IsNullOrWhiteSpace($ExecutablePath)) {
        throw 'An executable path is required for the profile sidecar contract.'
    }
    if (-not [StringComparer]::OrdinalIgnoreCase.Equals([IO.Path]::GetFileName($ExecutablePath), 'sakura.exe')) {
        throw 'The profile sidecar contract requires an executable named sakura.exe.'
    }
    return Join-Path (Split-Path -Parent ([IO.Path]::GetFullPath($ExecutablePath))) $script:StartupProfileSidecarFileName
}

function Assert-StartupProfileSidecar([string]$ExecutablePath) {
    $exePath = [IO.Path]::GetFullPath($ExecutablePath)
    if (-not [IO.File]::Exists($exePath)) { throw 'The bundled sakura.exe is missing.' }
    $exeAttributes = [IO.File]::GetAttributes($exePath)
    if (($exeAttributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw 'The bundled sakura.exe may not be a reparse point.'
    }
    $sidecarPath = Get-StartupProfileSidecarPath $exePath
    if (-not [IO.File]::Exists($sidecarPath)) {
        throw 'The bundled sakura.exe.ini sidecar is missing; refusing a user-profile fallback.'
    }
    $sidecarAttributes = [IO.File]::GetAttributes($sidecarPath)
    if (($sidecarAttributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw 'The bundled sakura.exe.ini sidecar may not be a reparse point.'
    }
    $expected = Get-StartupProfileSidecarContractIdentity
    $actualBytes = [IO.File]::ReadAllBytes($sidecarPath)
    $actualHash = Get-ByteArraySha256 $actualBytes
    if ([UInt64]$actualBytes.Length -ne $expected.sizeBytes -or $actualHash -ne $expected.sha256) {
        throw 'The bundled sakura.exe.ini sidecar is not the exact MultiUser=0 contract.'
    }
    return [pscustomobject][ordered]@{
        contract = $expected.contract
        fileName = $expected.fileName
        sha256 = $actualHash
        sizeBytes = [UInt64]$actualBytes.Length
        multiUser = 0
        verified = $true
    }
}

function Get-StartupFileIdentity([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path) -or -not [IO.File]::Exists($Path)) {
        throw 'A required artifact file is missing.'
    }
    $item = Get-Item -LiteralPath $Path -Force -ErrorAction Stop
    if ($item -isnot [IO.FileInfo] -or (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
        throw 'An artifact must be a regular non-reparse file.'
    }
    return [pscustomobject][ordered]@{
        path = [IO.Path]::GetFullPath($Path)
        sha256 = Get-Sha256 $Path
        sizeBytes = [UInt64]$item.Length
    }
}

function Convert-StartupArtifactRelativePath([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) { throw 'An artifact manifest path must be non-empty.' }
    $candidate = $Path.Replace('/', '\').Trim()
    if ([IO.Path]::IsPathRooted($candidate) -or $candidate.Contains(':')) {
        throw 'An artifact manifest path must be relative.'
    }
    $parts = @($candidate -split '\\')
    if ($parts.Count -eq 0 -or @($parts | Where-Object { [string]::IsNullOrWhiteSpace($_) -or $_ -eq '.' -or $_ -eq '..' }).Count -gt 0) {
        throw 'An artifact manifest path contains an unsafe component.'
    }
    return ($parts -join '\')
}

function Assert-StartupNonReparsePath([string]$Path, [string]$Root) {
    $fullPath = [IO.Path]::GetFullPath($Path)
    $fullRoot = [IO.Path]::GetFullPath($Root).TrimEnd('\')
    if (-not (Test-SamePath $fullPath $fullRoot) -and -not $fullPath.StartsWith($fullRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'An artifact path escaped its declared root.'
    }
    $current = $fullPath
    while ($true) {
        if ([IO.File]::Exists($current) -or [IO.Directory]::Exists($current)) {
            $attributes = [IO.File]::GetAttributes($current)
            if (($attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "An artifact path may not contain a reparse point: $current"
            }
        }
        if (Test-SamePath $current $fullRoot) { break }
        $parent = Split-Path -Parent $current
        if ([string]::IsNullOrWhiteSpace($parent) -or (Test-SamePath $parent $current)) { throw 'An artifact path has no valid root.' }
        $current = $parent
    }
}

function Convert-StartupReceiptPath([string]$Path, [string]$FieldName) {
    if ([string]::IsNullOrWhiteSpace($Path)) { throw "A runtime receipt $FieldName must be non-empty." }
    $normalized = $Path.Replace('/', '\')
    if ($normalized -ne $normalized.Trim() -or $normalized.IndexOf([char]0) -ge 0) {
        throw "A runtime receipt $FieldName contains unsafe whitespace or control data."
    }
    if ([IO.Path]::IsPathRooted($normalized) -or $normalized.IndexOf(':') -ge 0) {
        throw "A runtime receipt $FieldName must be relative and may not contain an alternate data stream."
    }
    $parts = @($normalized -split '\\')
    if ($parts.Count -eq 0 -or @($parts | Where-Object {
            [string]::IsNullOrWhiteSpace($_) -or $_ -eq '.' -or $_ -eq '..' -or $_ -match '[\x00-\x1f<>\"|?*]' -or
            $_ -match '[ \.]$' -or $_ -match '^(?i:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\..*)?$'
        }).Count -gt 0) {
        throw "A runtime receipt $FieldName contains an unsafe path component."
    }
    return ($parts -join '\')
}

function Get-StartupReceiptPathBinding([string]$Destination, [string]$Source, [string]$Context) {
    if ($Context -cne 'msvc-x64-debug' -and $Context -cne 'msvc-x64-release') {
        throw 'A runtime receipt context must be the canonical MSVC x64 context.'
    }
    $destinationPath = Convert-StartupReceiptPath $Destination 'destination'
    $sourcePath = Convert-StartupReceiptPath $Source 'source'
    $destinationPrefix = 'build/staging/{0}/sakura-editor/' -f $Context
    $destinationPrefix = $destinationPrefix.Replace('/', '\')
    $sourceConfiguration = if ($Context -ceq 'msvc-x64-debug') { 'Debug' } else { 'Release' }
    $sourcePrefix = ('x64/{0}/' -f $sourceConfiguration).Replace('/', '\')
    if (-not $destinationPath.StartsWith($destinationPrefix, [StringComparison]::Ordinal) -or
        -not $sourcePath.StartsWith($sourcePrefix, [StringComparison]::Ordinal)) {
        throw 'A runtime receipt source or destination is outside its canonical stage prefix.'
    }
    $destinationSuffix = $destinationPath.Substring($destinationPrefix.Length)
    $sourceSuffix = $sourcePath.Substring($sourcePrefix.Length)
    if ([string]::IsNullOrWhiteSpace($destinationSuffix) -or
        -not [StringComparer]::Ordinal.Equals($destinationSuffix, $sourceSuffix)) {
        throw 'A runtime receipt source and destination do not identify the same relative file.'
    }
    return [pscustomobject][ordered]@{
        destination = $destinationPath
        source = $sourcePath
        relativePath = $destinationSuffix
    }
}

function Assert-StartupReceiptArtifactIdentity([string]$ArtifactId, [string]$Role, [string]$RelativePath, [string]$Context) {
    if ([string]::IsNullOrWhiteSpace($ArtifactId) -or $ArtifactId -match '[\r\n]' -or
        [string]::IsNullOrWhiteSpace($Role) -or $Role -match '[\r\n]') {
        throw 'A runtime receipt artifact identity must contain non-empty artifact_id and role values.'
    }
    if ([StringComparer]::Ordinal.Equals($RelativePath, 'sakura.exe')) {
        if (-not [StringComparer]::Ordinal.Equals($Role, 'editor') -or
            -not [StringComparer]::Ordinal.Equals($ArtifactId, ('sakura-editor-{0}-product' -f $Context))) {
            throw 'The runtime receipt editor identity is not canonical.'
        }
        return
    }
    if ([StringComparer]::Ordinal.Equals($Role, 'editor') -or
        [StringComparer]::Ordinal.Equals($ArtifactId, ('sakura-editor-{0}-product' -f $Context))) {
        throw 'Only the canonical sakura.exe entry may use the editor identity.'
    }
    $knownLanguages = @{
        'sakura_lang_en_US.dll' = [pscustomobject]@{ artifactId = 'sakura-language-en-us-resource'; role = 'language-en-us' }
        'sakura_lang_zh_CN.dll' = [pscustomobject]@{ artifactId = 'sakura-language-zh-cn-resource'; role = 'language-zh-cn' }
    }
    if (($ArtifactId -ceq 'sakura-language-en-us-resource' -or $Role -ceq 'language-en-us') -and
        -not [StringComparer]::Ordinal.Equals($RelativePath, 'sakura_lang_en_US.dll')) {
        throw 'The runtime receipt en-US language identity must use its canonical top-level path.'
    }
    if (($ArtifactId -ceq 'sakura-language-zh-cn-resource' -or $Role -ceq 'language-zh-cn') -and
        -not [StringComparer]::Ordinal.Equals($RelativePath, 'sakura_lang_zh_CN.dll')) {
        throw 'The runtime receipt zh-CN language identity must use its canonical top-level path.'
    }
    if ($knownLanguages.ContainsKey($RelativePath)) {
        $expected = $knownLanguages[$RelativePath]
        if (-not [StringComparer]::Ordinal.Equals($ArtifactId, $expected.artifactId) -or
            -not [StringComparer]::Ordinal.Equals($Role, $expected.role)) {
            throw 'The runtime receipt language artifact identity is not canonical.'
        }
    }
}

function Get-StartupArtifactClosureManifest([string]$ArtifactRoot, [string]$ReceiptPath = $null) {
    if ([string]::IsNullOrWhiteSpace($ArtifactRoot)) { throw 'An artifact directory is required.' }
    $root = [IO.Path]::GetFullPath($ArtifactRoot)
    if (-not [IO.Directory]::Exists($root)) { throw 'The artifact directory does not exist.' }
    $rootAttributes = [IO.File]::GetAttributes($root)
    if (($rootAttributes -band [IO.FileAttributes]::Directory) -eq 0 -or ($rootAttributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw 'The artifact root must be a regular non-reparse directory.'
    }
    if ([string]::IsNullOrWhiteSpace($ReceiptPath)) { $ReceiptPath = Join-Path $root '.sakura-runtime-stage.json' }
    $receipt = Get-StartupFileIdentity ([IO.Path]::GetFullPath($ReceiptPath))
    Assert-StartupNonReparsePath $receipt.path $root
    try { $raw = [IO.File]::ReadAllText($receipt.path) | ConvertFrom-Json } catch { throw 'The runtime artifact receipt is not valid JSON.' }
    if ($null -eq $raw -or $null -eq $raw.schema_version -or [int]$raw.schema_version -ne 1 -or
        [string]::IsNullOrWhiteSpace([string]$raw.context_id) -or [string]::IsNullOrWhiteSpace([string]$raw.staging_set_id)) {
        throw 'The runtime artifact receipt has an unsupported schema.'
    }
    $context = [string]$raw.context_id
    if ($context -cne 'msvc-x64-debug' -and $context -cne 'msvc-x64-release') {
        throw 'The runtime artifact receipt context is not canonical.'
    }
    $rawFiles = @($raw.files)
    if ($rawFiles.Count -eq 0) { throw 'The runtime artifact receipt declares no files.' }
    $files = New-Object Collections.Generic.List[object]
    $paths = @{}
    $editorCount = 0
    foreach ($rawFile in $rawFiles) {
        foreach ($required in @('artifact_id', 'source', 'destination', 'role', 'sha256', 'size')) {
            if ($null -eq $rawFile.PSObject.Properties[$required]) { throw 'The runtime artifact receipt has an incomplete file entry.' }
        }
        $binding = Get-StartupReceiptPathBinding ([string]$rawFile.destination) ([string]$rawFile.source) $context
        $relativePath = Convert-StartupArtifactRelativePath $binding.relativePath
        $artifactId = [string]$rawFile.artifact_id
        $role = [string]$rawFile.role
        Assert-StartupReceiptArtifactIdentity $artifactId $role $relativePath $context
        $key = $binding.destination.ToUpperInvariant()
        if ($paths.ContainsKey($key)) { throw 'The runtime artifact receipt declares a duplicate file.' }
        $paths[$key] = $true
        $expectedHash = [string]$rawFile.sha256
        if ($expectedHash -notmatch '^(?i:sha256:)?[0-9a-f]{64}$') { throw 'The runtime artifact receipt contains an invalid SHA-256.' }
        $expectedHash = $expectedHash -replace '^(?i:sha256:)', ''
        [UInt64]$expectedSize = 0
        try { $expectedSize = [UInt64]$rawFile.size } catch { throw 'The runtime artifact receipt contains an invalid file size.' }
        $sourcePath = Join-Path $root $relativePath
        Assert-StartupNonReparsePath $sourcePath $root
        $identity = Get-StartupFileIdentity $sourcePath
        if ($identity.sha256 -ne $expectedHash -or [UInt64]$identity.sizeBytes -ne $expectedSize) {
            throw 'A runtime artifact receipt entry does not match its staged file.'
        }
        if ([StringComparer]::OrdinalIgnoreCase.Equals($role, 'editor')) { ++$editorCount }
        [void]$files.Add([pscustomobject][ordered]@{
            relativePath = $relativePath
            canonicalRelativePath = $binding.destination
            sourcePath = $identity.path
            artifactId = $artifactId
            role = $role
            sha256 = $expectedHash
            sizeBytes = $expectedSize
            source = $binding.source
            destination = $binding.destination
        })
    }
    if ($editorCount -ne 1) { throw 'The runtime artifact receipt must declare exactly one editor file.' }
    $executable = @($files | Where-Object { [StringComparer]::OrdinalIgnoreCase.Equals([string]$_.role, 'editor') })[0]
    if (-not [StringComparer]::OrdinalIgnoreCase.Equals($executable.relativePath, 'sakura.exe')) {
        throw 'The runtime artifact receipt editor file must be the bundle-root sakura.exe.'
    }
    return [pscustomobject][ordered]@{
        mode = 'runtime-stage-receipt'
        schemaVersion = [int]$raw.schema_version
        contextId = $context
        stagingSetId = [string]$raw.staging_set_id
        rootPath = $root
        receipt = $receipt
        files = $files.ToArray()
        executableRelativePath = [string]$executable.relativePath
        executablePath = [string]$executable.sourcePath
    }
}

function Assert-StartupArtifactBundlePath([string]$BundlePath, [string]$BundleRoot, [string]$BundleName) {
    if ([string]::IsNullOrWhiteSpace($BundlePath) -or [string]::IsNullOrWhiteSpace($BundleRoot) -or [string]::IsNullOrWhiteSpace($BundleName)) {
        throw 'An artifact bundle path, root, and name are required.'
    }
    $resolvedBundle = Get-NormalizedPath $BundlePath
    $resolvedRoot = Get-NormalizedPath $BundleRoot
    if ((Split-Path -Parent $resolvedBundle) -ne $resolvedRoot -or
        [IO.Path]::GetFileName($resolvedBundle) -ne $BundleName.ToUpperInvariant() -or
        -not $BundleName.StartsWith('startup-probe-bundle-', [StringComparison]::Ordinal)) {
        throw 'Refusing an unsafe artifact bundle path.'
    }
    if (Test-Path -LiteralPath $BundleRoot) {
        $rootItem = Get-Item -LiteralPath $BundleRoot -Force -ErrorAction Stop
        if ($rootItem -isnot [IO.DirectoryInfo] -or (($rootItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
            throw 'The artifact bundle root must be a regular non-reparse directory.'
        }
    }
}

function New-StartupArtifactBundle([string]$SourcePath, [string]$BundleRoot, [string]$BundleName, [string]$ClosureReceiptPath = $null) {
    $sourceItem = Get-Item -LiteralPath $SourcePath -Force -ErrorAction Stop
    $closure = $null
    if ($sourceItem -is [IO.DirectoryInfo]) {
        if (($sourceItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'The artifact source directory may not be a reparse point.' }
        $closure = Get-StartupArtifactClosureManifest $sourceItem.FullName $ClosureReceiptPath
        $source = Get-StartupFileIdentity $closure.executablePath
    }
    else {
        $source = Get-StartupFileIdentity $SourcePath
        if (-not [string]::IsNullOrWhiteSpace($ClosureReceiptPath)) {
            $closure = Get-StartupArtifactClosureManifest (Split-Path -Parent $source.path) $ClosureReceiptPath
            if (-not (Test-SamePath $closure.executablePath $source.path)) { throw 'The receipt editor file does not match the supplied executable.' }
        }
        else {
            $closure = [pscustomobject][ordered]@{
                mode = 'exe-only'
                schemaVersion = $null
                contextId = $null
                stagingSetId = $null
                rootPath = Split-Path -Parent $source.path
                receipt = $null
                files = @([pscustomobject][ordered]@{ relativePath = 'sakura.exe'; canonicalRelativePath = 'sakura.exe'; sourcePath = $source.path; role = 'editor'; sha256 = $source.sha256; sizeBytes = [UInt64]$source.sizeBytes; source = $source.path; destination = 'sakura.exe' })
                executableRelativePath = 'sakura.exe'
                executablePath = $source.path
            }
        }
    }
    if (-not (Test-Path -LiteralPath $BundleRoot -PathType Container)) {
        throw 'The artifact bundle root must already exist and be owned by the measurement.'
    }
    $bundlePath = Join-Path ([IO.Path]::GetFullPath($BundleRoot)) $BundleName
    Assert-StartupArtifactBundlePath $bundlePath $BundleRoot $BundleName
    if (Test-Path -LiteralPath $bundlePath) { throw 'The generated artifact bundle already exists.' }
    [IO.Directory]::CreateDirectory($bundlePath) | Out-Null
    try {
        $executablePath = Join-Path $bundlePath 'sakura.exe'
        $copiedFiles = New-Object Collections.Generic.List[object]
        foreach ($closureFile in @($closure.files)) {
            $destinationPath = Join-Path $bundlePath $closureFile.relativePath
            Assert-StartupNonReparsePath $destinationPath $bundlePath
            [IO.Directory]::CreateDirectory((Split-Path -Parent $destinationPath)) | Out-Null
            [IO.File]::Copy($closureFile.sourcePath, $destinationPath, $false)
            $copiedFile = Get-StartupFileIdentity $destinationPath
            if ($copiedFile.sha256 -ne $closureFile.sha256 -or [UInt64]$copiedFile.sizeBytes -ne [UInt64]$closureFile.sizeBytes) {
                throw 'A copied runtime artifact did not retain its manifest identity.'
            }
            [void]$copiedFiles.Add([pscustomobject][ordered]@{
                relativePath = [string]$closureFile.relativePath
                canonicalRelativePath = if ($null -eq $closureFile.canonicalRelativePath) { [string]$closureFile.relativePath } else { [string]$closureFile.canonicalRelativePath }
                sourcePath = [string]$closureFile.sourcePath
                copiedPath = [string]$copiedFile.path
                role = [string]$closureFile.role
                sha256 = [string]$closureFile.sha256
                sizeBytes = [UInt64]$closureFile.sizeBytes
            })
        }
        if (-not (Test-Path -LiteralPath $executablePath -PathType Leaf)) { throw 'The runtime artifact closure did not provide sakura.exe.' }
        $copied = Get-StartupFileIdentity $executablePath
        if ($copied.sha256 -ne $source.sha256 -or $copied.sizeBytes -ne $source.sizeBytes) {
            throw 'The copied sakura.exe did not retain the source artifact identity.'
        }
        $sidecarPath = Join-Path $bundlePath $script:StartupProfileSidecarFileName
        $encoding = [Text.UnicodeEncoding]::new($false, $true)
        [IO.File]::WriteAllText($sidecarPath, $script:StartupProfileSidecarText, $encoding)
        $sidecar = Assert-StartupProfileSidecar $executablePath
        return [pscustomobject][ordered]@{
            source = $source
            copied = $copied
            closure = [pscustomobject][ordered]@{
                mode = [string]$closure.mode
                schemaVersion = $closure.schemaVersion
                contextId = $closure.contextId
                stagingSetId = $closure.stagingSetId
                sourceRoot = [string]$closure.rootPath
                receipt = $closure.receipt
                receiptSha256 = if ($null -eq $closure.receipt) { $null } else { [string]$closure.receipt.sha256 }
                receiptSizeBytes = if ($null -eq $closure.receipt) { $null } else { [UInt64]$closure.receipt.sizeBytes }
                files = @($closure.files)
                copiedFiles = $copiedFiles.ToArray()
                executableRelativePath = [string]$closure.executableRelativePath
            }
            sidecar = $sidecar
            bundlePath = [IO.Path]::GetFullPath($bundlePath)
            bundleRoot = [IO.Path]::GetFullPath($BundleRoot)
            bundleName = $BundleName
            executablePath = [IO.Path]::GetFullPath($executablePath)
            cleanupVerified = $false
        }
    }
    catch {
        # Remove only the exact generated bundle after a partial copy.  The root
        # itself is caller-owned and is never traversed or removed here.
        if ($null -ne $bundlePath -and (Test-Path -LiteralPath $bundlePath)) {
            $partial = [pscustomobject][ordered]@{
                bundlePath = $bundlePath
                bundleRoot = [IO.Path]::GetFullPath($BundleRoot)
                bundleName = $BundleName
            }
            try { Remove-StartupArtifactBundle $partial } catch { }
        }
        throw
    }
}

function Get-StartupArtifactBundleVerification([object]$Bundle) {
    if ($null -eq $Bundle) { throw 'An artifact bundle is required.' }
    Assert-StartupArtifactBundlePath $Bundle.bundlePath $Bundle.bundleRoot $Bundle.bundleName
    $sourceAfter = Get-StartupFileIdentity $Bundle.source.path
    $copiedAfter = Get-StartupFileIdentity $Bundle.executablePath
    $sidecarAfter = Assert-StartupProfileSidecar $Bundle.executablePath
    $sourceClosureUnchanged = $true
    $copiedClosureUnchanged = $true
    $closureEntries = New-Object Collections.Generic.List[object]
    foreach ($closureFile in @($Bundle.closure.files)) {
        $sourceCurrent = Get-StartupFileIdentity $closureFile.sourcePath
        $copiedEntry = @($Bundle.closure.copiedFiles | Where-Object { $_.relativePath -eq $closureFile.relativePath })
        if ($copiedEntry.Count -ne 1) { throw 'The copied runtime artifact closure is incomplete.' }
        $copiedCurrent = Get-StartupFileIdentity $copiedEntry[0].copiedPath
        $sourceMatches = $sourceCurrent.sha256 -eq $closureFile.sha256 -and [UInt64]$sourceCurrent.sizeBytes -eq [UInt64]$closureFile.sizeBytes
        $copiedMatches = $copiedCurrent.sha256 -eq $closureFile.sha256 -and [UInt64]$copiedCurrent.sizeBytes -eq [UInt64]$closureFile.sizeBytes
        $sourceClosureUnchanged = $sourceClosureUnchanged -and $sourceMatches
        $copiedClosureUnchanged = $copiedClosureUnchanged -and $copiedMatches
        [void]$closureEntries.Add([ordered]@{ relativePath = $closureFile.relativePath; canonicalRelativePath = if ($null -eq $closureFile.canonicalRelativePath) { [string]$closureFile.relativePath } else { [string]$closureFile.canonicalRelativePath }; role = $closureFile.role; sourceSha256 = $sourceCurrent.sha256; copiedSha256 = $copiedCurrent.sha256; sourceMatches = [bool]$sourceMatches; copiedMatches = [bool]$copiedMatches })
    }
    $receiptUnchanged = $true
    if ($null -ne $Bundle.closure.receipt) {
        $receiptCurrent = Get-StartupFileIdentity $Bundle.closure.receipt.path
        $receiptUnchanged = $receiptCurrent.sha256 -eq $Bundle.closure.receipt.sha256 -and [UInt64]$receiptCurrent.sizeBytes -eq [UInt64]$Bundle.closure.receipt.sizeBytes
    }
    return [pscustomobject][ordered]@{
        sourceHashBefore = $Bundle.source.sha256
        sourceHashAfter = $sourceAfter.sha256
        sourceSizeBefore = [UInt64]$Bundle.source.sizeBytes
        sourceSizeAfter = [UInt64]$sourceAfter.sizeBytes
        sourceUnchanged = $sourceAfter.sha256 -eq $Bundle.source.sha256 -and $sourceAfter.sizeBytes -eq $Bundle.source.sizeBytes
        copiedHashBefore = $Bundle.copied.sha256
        copiedHashAfter = $copiedAfter.sha256
        copiedSizeBefore = [UInt64]$Bundle.copied.sizeBytes
        copiedSizeAfter = [UInt64]$copiedAfter.sizeBytes
        copiedUnchanged = $copiedAfter.sha256 -eq $Bundle.copied.sha256 -and $copiedAfter.sizeBytes -eq $Bundle.copied.sizeBytes
        closureMode = [string]$Bundle.closure.mode
        sourceClosureUnchanged = [bool]($sourceClosureUnchanged -and $receiptUnchanged)
        copiedClosureUnchanged = [bool]$copiedClosureUnchanged
        receiptUnchanged = [bool]$receiptUnchanged
        closureFiles = $closureEntries.ToArray()
        closureFileCount = [int]$closureEntries.Count
        sidecarSha256 = $sidecarAfter.sha256
        sidecarSizeBytes = [UInt64]$sidecarAfter.sizeBytes
        sidecarVerified = [bool]$sidecarAfter.verified
        cleanupVerified = $false
    }
}

function Assert-StartupArtifactBundleUnchanged([object]$Bundle) {
    $verification = Get-StartupArtifactBundleVerification $Bundle
    if (-not $verification.sourceUnchanged -or -not $verification.copiedUnchanged -or -not $verification.sourceClosureUnchanged -or
        -not $verification.copiedClosureUnchanged -or -not $verification.sidecarVerified) {
        throw 'The source or copied startup artifact changed during measurement.'
    }
    return $verification
}

function Remove-StartupArtifactBundle([object]$Bundle) {
    if ($null -eq $Bundle) { return }
    Assert-StartupArtifactBundlePath $Bundle.bundlePath $Bundle.bundleRoot $Bundle.bundleName
    if (-not (Test-Path -LiteralPath $Bundle.bundlePath)) { return }
    $item = Get-Item -LiteralPath $Bundle.bundlePath -Force -ErrorAction Stop
    if ($item -isnot [IO.DirectoryInfo] -or (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
        throw 'Refusing to remove a non-owned or reparse artifact bundle.'
    }
    [void](Get-SafeDirectoryTreeEntries $Bundle.bundlePath)
    try {
        [IO.Directory]::Delete($Bundle.bundlePath, $true)
    }
    catch [UnauthorizedAccessException] {
        foreach ($entry in @(Get-SafeDirectoryTreeEntries $Bundle.bundlePath | Where-Object { -not $_.IsDirectory })) {
            if (($entry.Attributes -band [IO.FileAttributes]::ReadOnly) -ne 0) {
                [IO.File]::SetAttributes($entry.FullName, $entry.Attributes -band (-bnot [IO.FileAttributes]::ReadOnly))
            }
        }
        [IO.Directory]::Delete($Bundle.bundlePath, $true)
    }
    if (Test-Path -LiteralPath $Bundle.bundlePath) { throw 'The artifact bundle survived cleanup.' }
}

function Get-SafeDirectoryTreeEntries([string]$DirectoryPath) {
    if (-not [IO.Directory]::Exists($DirectoryPath)) { return @() }
    $root = [IO.Path]::GetFullPath($DirectoryPath)
    $rootAttributes = [IO.File]::GetAttributes($root)
    if (($rootAttributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw 'Refusing to recurse into a directory reparse point.'
    }
    if (($rootAttributes -band [IO.FileAttributes]::Directory) -eq 0) {
        throw 'Expected a directory while walking a profile tree.'
    }
    $pending = New-Object Collections.Generic.Stack[string]
    $entries = New-Object Collections.Generic.List[object]
    [void]$pending.Push($root)
    while ($pending.Count -gt 0) {
        $current = $pending.Pop()
        foreach ($entryPath in [IO.Directory]::EnumerateFileSystemEntries($current)) {
            $attributes = [IO.File]::GetAttributes($entryPath)
            if (($attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw 'Refusing to recurse into a profile reparse point.'
            }
            $isDirectory = ($attributes -band [IO.FileAttributes]::Directory) -ne 0
            [void]$entries.Add([pscustomobject][ordered]@{
                FullName = [IO.Path]::GetFullPath($entryPath)
                Attributes = $attributes
                IsDirectory = [bool]$isDirectory
                Length = if ($isDirectory) { [UInt64]0 } else { [UInt64]([IO.FileInfo]$entryPath).Length }
            })
            if ($isDirectory) { [void]$pending.Push($entryPath) }
        }
    }
    return $entries.ToArray()
}

function New-StartupTraceDirectory([string]$OutputPath, [string]$RunId, [int]$Iteration, [string]$Condition) {
    $name = 'startup-trace-{0}-iteration-{1}-{2}' -f $RunId, $Iteration, $Condition
    $traceDirectory = Join-Path $OutputPath $name
    if (Test-Path -LiteralPath $traceDirectory) {
        throw "Generated startup trace directory already exists: $traceDirectory"
    }
    [IO.Directory]::CreateDirectory($traceDirectory) | Out-Null
    return [IO.Path]::GetFullPath($traceDirectory)
}

function Get-StartupTracePhaseDurations($Records) {
    $pairs = @(
        [ordered]@{ phase = 'factory'; begin = 'factory_begin'; end = 'factory_end' },
        [ordered]@{ phase = 'control_spawn'; begin = 'control_spawn_begin'; end = 'control_spawn_end' },
        [ordered]@{ phase = 'control_wait'; begin = 'control_wait_begin'; end = 'control_wait_end' },
        [ordered]@{ phase = 'control_ready_event'; begin = 'control_ready_event_begin'; end = 'control_ready_event_end' },
        [ordered]@{ phase = 'editor_spawn'; begin = 'editor_spawn_begin'; end = 'editor_spawn_end' },
        [ordered]@{ phase = 'editor_wait'; begin = 'editor_wait_begin'; end = 'editor_wait_end' },
		[ordered]@{ phase = 'editor_ready_event'; begin = 'editor_ready_event_begin'; end = 'editor_ready_event_end' },
		[ordered]@{ phase = 'uipi_check'; begin = 'uipi_check_begin'; end = 'uipi_check_end' },
		[ordered]@{ phase = 'read'; begin = 'read_begin'; end = 'read_end' },
        [ordered]@{ phase = 'layout'; begin = 'layout_begin'; end = 'layout_complete' },
		[ordered]@{ phase = 'startup_document'; begin = 'startup_document_armed'; end = 'startup_document_complete' },
        [ordered]@{ phase = 'startup_draw_commit'; begin = 'startup_draw_commit_begin'; end = 'startup_draw_commit_end' },
		[ordered]@{ phase = 'startup_draw_layout'; begin = 'startup_draw_layout_begin'; end = 'startup_draw_layout_end' },
		[ordered]@{ phase = 'startup_draw_scroll'; begin = 'startup_draw_scroll_begin'; end = 'startup_draw_scroll_end' },
		[ordered]@{ phase = 'startup_draw_show'; begin = 'startup_draw_show_begin'; end = 'startup_draw_show_end' },
		[ordered]@{ phase = 'startup_draw_redraw'; begin = 'startup_draw_redraw_begin'; end = 'startup_draw_redraw_end' },
		[ordered]@{ phase = 'first_content_paint'; begin = 'first_content_paint_begin'; end = 'first_content_paint_end' },
		[ordered]@{ phase = 'first_content_paint_prepare'; begin = 'first_content_paint_prepare_begin'; end = 'first_content_paint_prepare_end' },
		[ordered]@{ phase = 'first_content_paint_lines'; begin = 'first_content_paint_lines_begin'; end = 'first_content_paint_lines_end' },
		[ordered]@{ phase = 'first_content_paint_finish'; begin = 'first_content_paint_finish_begin'; end = 'first_content_paint_finish_end' },
        # These boundaries are recorded in different processes. Match the known
        # producer/consumer roles instead of process IDs. Start immediately before
        # SetEvent because the waiter can run before the signaling process records
        # its post-call marker.
        [ordered]@{ phase = 'control_ready_handoff'; begin = 'control_ready_event_begin'; beginRole = 'control'; end = 'control_wait_end'; endRole = 'editor' },
        [ordered]@{ phase = 'editor_ready_handoff'; begin = 'editor_ready_event_begin'; beginRole = 'editor'; end = 'editor_wait_end'; endRole = 'control' }
    )
    $durations = New-Object Collections.Generic.List[object]
    foreach ($pair in $pairs) {
        foreach ($begin in @($Records | Where-Object { $_.event -eq $pair.begin })) {
            if ($pair.Contains('beginRole') -and $begin.role -ne $pair.beginRole) { continue }
            $end = @($Records | Where-Object {
                $_.event -eq $pair.end -and
                $_.qpc -ge $begin.qpc -and
                ($pair.Contains('endRole') -eq $false -or $_.role -eq $pair.endRole) -and
                ($pair.Contains('endRole') -or $_.pid -eq $begin.pid)
            } | Select-Object -First 1)
            if ($end.Count -eq 0 -or $end[0].frequency -ne $begin.frequency) { continue }
            $durations.Add([pscustomobject][ordered]@{
                phase = $pair.phase
                pid = $begin.pid
                role = $begin.role
                endPid = $end[0].pid
                endRole = $end[0].role
                beginEvent = $pair.begin
                endEvent = $pair.end
                durationMs = [Math]::Round((($end[0].qpc - $begin.qpc) * 1000.0) / $begin.frequency, 3)
            })
        }
    }
    return @($durations.ToArray())
}

function Get-StartupTraceSummary([string]$TraceDirectory, [int64]$LaunchQpc, [int64]$LaunchFrequency) {
    $traceBounds = Get-StartupTraceBounds
    $summary = [ordered]@{
        enabled = $true
        collected = $false
        directory = $TraceDirectory
        launchQpc = $LaunchQpc
        launchFrequency = $LaunchFrequency
        files = @()
        fileCount = 0
        totalBytes = [Int64]0
        lineCount = [Int64]0
        maxLineLength = 0
        validRecordCount = 0
        invalidLineCount = 0
        parseErrors = @()
        records = @()
        majorPhases = @()
        phaseDurations = @()
        firstContentPainted = $null
        firstContentPaintedMs = $null
        firstContentPaintMetrics = $null
        startupMiniMapMetrics = $null
        clockCompatible = $false
    }
    if (-not (Test-Path -LiteralPath $TraceDirectory -PathType Container)) {
        $summary.parseErrors = @([ordered]@{ file = $null; line = $null; error = 'Startup trace directory was not created.' })
        return [pscustomobject]$summary
    }

    try {
        $traceDirectoryAttributes = [IO.File]::GetAttributes($TraceDirectory)
        if (($traceDirectoryAttributes -band [IO.FileAttributes]::Directory) -eq 0 -or
            ($traceDirectoryAttributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            $summary.parseErrors = @([ordered]@{ file = $null; line = $null; error = 'StartupTraceDirectoryNotRegular' })
            return [pscustomobject]$summary
        }
    }
    catch {
        $summary.parseErrors = @([ordered]@{ file = $null; line = $null; error = 'StartupTraceDirectoryUnavailable' })
        return [pscustomobject]$summary
    }

    $traceFiles = New-Object Collections.Generic.List[object]
    $traceEnumerator = $null
    try {
        # Enumerate only matching files in the directory itself.  Do not recurse
        # or materialize an attacker-controlled list before applying the bound.
        $traceEnumerator = [IO.Directory]::EnumerateFiles($TraceDirectory, 'startup-trace-*.jsonl', [IO.SearchOption]::TopDirectoryOnly).GetEnumerator()
        while ($traceEnumerator.MoveNext()) {
            $entryPath = [string]$traceEnumerator.Current
            $entryName = [IO.Path]::GetFileName($entryPath)
            $entryAttributes = [IO.File]::GetAttributes($entryPath)
            if (($entryAttributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                $summary.parseErrors = @([ordered]@{ file = $entryName; line = $null; error = 'StartupTraceEntryNotRegular' })
                $summary.collected = $false
                return [pscustomobject]$summary
            }
            if (($entryAttributes -band [IO.FileAttributes]::Directory) -ne 0) {
                continue
            }
            if ($traceFiles.Count -ge [int]$traceBounds.maxFiles) {
                $summary.parseErrors = @([ordered]@{ file = $entryName; line = $null; error = 'StartupTraceBoundsExceeded:files' })
                $summary.collected = $false
                return [pscustomobject]$summary
            }
            $entryLength = [IO.FileInfo]$entryPath
            $entryBytes = [Int64]$entryLength.Length
            if ($entryBytes -gt [Int64]$traceBounds.maxBytes - $summary.totalBytes) {
                $summary.parseErrors = @([ordered]@{ file = $entryName; line = $null; error = 'StartupTraceBoundsExceeded:bytes' })
                $summary.collected = $false
                return [pscustomobject]$summary
            }
            $summary.totalBytes = [Int64]($summary.totalBytes + $entryBytes)
            [void]$traceFiles.Add([pscustomobject]@{ path = $entryPath; name = $entryName; bytes = $entryBytes })
        }
    }
    catch {
        $summary.parseErrors = @([ordered]@{ file = $null; line = $null; error = 'StartupTraceEnumerationFailed' })
        $summary.collected = $false
        return [pscustomobject]$summary
    }
    finally {
        if ($null -ne $traceEnumerator) { $traceEnumerator.Dispose() }
    }
    $summary.fileCount = $traceFiles.Count
    if ($traceFiles.Count -eq 0) {
        $summary.parseErrors = @([ordered]@{ file = $null; line = $null; error = 'StartupTraceEmpty' })
        $summary.collected = $false
        return [pscustomobject]$summary
    }

    $records = New-Object Collections.Generic.List[object]
    foreach ($traceFile in @($traceFiles | Sort-Object name)) {
        $summary.files += $traceFile.name
        $lineNumber = 0
        $reader = $null
        try {
            $utf8Strict = New-Object Text.UTF8Encoding($false, $true)
            $reader = New-Object IO.StreamReader($traceFile.path, $utf8Strict, $true, 4096)
            while ($null -ne ($line = $reader.ReadLine())) {
                $lineNumber++
                $summary.lineCount = [Int64]($summary.lineCount + 1)
                if ($line.Length -gt $summary.maxLineLength) { $summary.maxLineLength = $line.Length }
                if ($line.Length -gt [int]$traceBounds.maxLineLength) {
                    $summary.parseErrors = @([ordered]@{ file = $traceFile.name; line = $lineNumber; error = 'StartupTraceBoundsExceeded:line-length' })
                    $summary.collected = $false
                    return [pscustomobject]$summary
                }
                if ($summary.lineCount -gt [Int64]$traceBounds.maxLines) {
                    $summary.parseErrors = @([ordered]@{ file = $traceFile.name; line = $lineNumber; error = 'StartupTraceBoundsExceeded:lines' })
                    $summary.collected = $false
                    return [pscustomobject]$summary
                }
                if ([string]::IsNullOrWhiteSpace($line)) { continue }
                try {
                    $raw = $line | ConvertFrom-Json
                    $names = @($raw.PSObject.Properties.Name)
                    $required = @('schemaVersion', 'qpc', 'frequency', 'pid', 'tid', 'role', 'event', 'value1', 'value2', 'detail')
                    foreach ($name in $required) {
                        if ($names -notcontains $name) { throw "Missing required field '$name'." }
                    }
                    if ([int]$raw.schemaVersion -ne 1) { throw "Unsupported schemaVersion '$($raw.schemaVersion)'." }
                    $record = [pscustomobject][ordered]@{
                        schemaVersion = 1
                        qpc = [int64]$raw.qpc
                        frequency = [int64]$raw.frequency
                        pid = [int]$raw.pid
                        tid = [int]$raw.tid
                        role = [string]$raw.role
                        event = [string]$raw.event
                        value1 = [int64]$raw.value1
                        value2 = [int64]$raw.value2
                        detail = [string]$raw.detail
                    }
                    if ($record.qpc -lt 0 -or $record.frequency -le 0 -or $record.pid -le 0 -or [string]::IsNullOrWhiteSpace($record.event)) {
                        throw 'Record contains an invalid timestamp, process id, frequency, or event.'
                    }
                    if ($records.Count -ge [int]$traceBounds.maxValidRecords) {
                        $summary.parseErrors = @([ordered]@{ file = $traceFile.name; line = $lineNumber; error = 'StartupTraceBoundsExceeded:records' })
                        $summary.collected = $false
                        return [pscustomobject]$summary
                    }
                    [void]$records.Add($record)
                }
                catch {
                    $summary.invalidLineCount++
                    $summary.parseErrors += [ordered]@{ file = $traceFile.name; line = $lineNumber; error = 'StartupTraceRecordInvalid' }
                }
            }
        }
        catch {
            $summary.parseErrors = @([ordered]@{ file = $traceFile.name; line = $lineNumber; error = 'StartupTraceReadFailed' })
            $summary.collected = $false
            return [pscustomobject]$summary
        }
        finally {
            if ($null -ne $reader) { $reader.Dispose() }
        }
    }

    $orderedRecords = @($records | Sort-Object qpc, pid, tid)
    $summary.validRecordCount = $orderedRecords.Count
    $summary.records = $orderedRecords
    # Application tracing emits only startup phase boundaries and decisions.  Keep the
    # complete ordered set so new phase names do not require a script release first.
    $summary.majorPhases = $orderedRecords
    $summary.phaseDurations = @(Get-StartupTracePhaseDurations $orderedRecords)
    $advanceWidth = @($orderedRecords | Where-Object { $_.event -eq 'first_content_advance_width_summary' } | Select-Object -First 1)
    $drawWidth = @($orderedRecords | Where-Object { $_.event -eq 'first_content_draw_width_summary' } | Select-Object -First 1)
    $textOutput = @($orderedRecords | Where-Object { $_.event -eq 'first_content_text_output_summary' } | Select-Object -First 1)
    $textVolume = @($orderedRecords | Where-Object { $_.event -eq 'first_content_text_volume_summary' } | Select-Object -First 1)
    $textBlock = @($orderedRecords | Where-Object { $_.event -eq 'first_content_text_block_summary' } | Select-Object -First 1)
    $textBlockFont = @($orderedRecords | Where-Object { $_.event -eq 'first_content_text_block_font_summary' } | Select-Object -First 1)
    $textBoundary = @($orderedRecords | Where-Object { $_.event -eq 'first_content_text_boundary_summary' } | Select-Object -First 1)
    $textScan = @($orderedRecords | Where-Object { $_.event -eq 'first_content_text_scan_summary' } | Select-Object -First 1)
    $nonBlockRange = @($orderedRecords | Where-Object { $_.event -eq 'first_content_nonblock_text_range_summary' } | Select-Object -First 1)
    $nonBlockRisk = @($orderedRecords | Where-Object { $_.event -eq 'first_content_nonblock_text_risk_summary' } | Select-Object -First 1)
    $nonBlockOther = @($orderedRecords | Where-Object { $_.event -eq 'first_content_nonblock_text_other_summary' } | Select-Object -First 1)
    if ($advanceWidth.Count -ne 0 -or $drawWidth.Count -ne 0 -or $textOutput.Count -ne 0 -or $textVolume.Count -ne 0 -or
        $textBlock.Count -ne 0 -or $textBlockFont.Count -ne 0 -or $textBoundary.Count -ne 0 -or $textScan.Count -ne 0 -or
        $nonBlockRange.Count -ne 0 -or $nonBlockRisk.Count -ne 0 -or $nonBlockOther.Count -ne 0) {
        $summary.firstContentPaintMetrics = [ordered]@{
            advanceWidthTicks = $(if ($advanceWidth.Count -ne 0) { [int64]$advanceWidth[0].value1 } else { $null })
            advanceWidthMs = $(if ($advanceWidth.Count -ne 0) { [Math]::Round(($advanceWidth[0].value1 * 1000.0) / $advanceWidth[0].frequency, 3) } else { $null })
            advanceWidthCalls = $(if ($advanceWidth.Count -ne 0) { [int64]$advanceWidth[0].value2 } else { $null })
            drawWidthTicks = $(if ($drawWidth.Count -ne 0) { [int64]$drawWidth[0].value1 } else { $null })
            drawWidthMs = $(if ($drawWidth.Count -ne 0) { [Math]::Round(($drawWidth[0].value1 * 1000.0) / $drawWidth[0].frequency, 3) } else { $null })
            drawWidthCalls = $(if ($drawWidth.Count -ne 0) { [int64]$drawWidth[0].value2 } else { $null })
            textOutputTicks = $(if ($textOutput.Count -ne 0) { [int64]$textOutput[0].value1 } else { $null })
            textOutputMs = $(if ($textOutput.Count -ne 0) { [Math]::Round(($textOutput[0].value1 * 1000.0) / $textOutput[0].frequency, 3) } else { $null })
            textOutputCalls = $(if ($textOutput.Count -ne 0) { [int64]$textOutput[0].value2 } else { $null })
            advanceUtf16Units = $(if ($textVolume.Count -ne 0) { [int64]$textVolume[0].value1 } else { $null })
            drawnUtf16Units = $(if ($textVolume.Count -ne 0) { [int64]$textVolume[0].value2 } else { $null })
            textBlockCount = $(if ($textBlock.Count -ne 0) { [int64]$textBlock[0].value1 } else { $null })
            textBlockUtf16Units = $(if ($textBlock.Count -ne 0) { [int64]$textBlock[0].value2 } else { $null })
            alternateFontBlockCount = $(if ($textBlockFont.Count -ne 0) { [int64]$textBlockFont[0].value1 } else { $null })
            maximumTextBlockUtf16Units = $(if ($textBlockFont.Count -ne 0) { [int64]$textBlockFont[0].value2 } else { $null })
            renderTypeBoundaryCount = $(if ($textBoundary.Count -ne 0) { [int64]([int64]$textBoundary[0].value1 -shr 32) } else { $null })
            lengthBoundaryCount = $(if ($textBoundary.Count -ne 0) { [int64]([int64]$textBoundary[0].value1 -band 0xffffffffL) } else { $null })
            colorBoundaryCount = $(if ($textBoundary.Count -ne 0) { [int64]([int64]$textBoundary[0].value2 -shr 32) } else { $null })
            tailBoundaryCount = $(if ($textBoundary.Count -ne 0) { [int64]([int64]$textBoundary[0].value2 -band 0xffffffffL) } else { $null })
            figureLookupCount = $(if ($textScan.Count -ne 0) { [int64]$textScan[0].value1 } else { $null })
            nonTextFigureCount = $(if ($textScan.Count -ne 0) { [int64]([int64]$textScan[0].value2 -shr 32) } else { $null })
            colorChangeCount = $(if ($textScan.Count -ne 0) { [int64]([int64]$textScan[0].value2 -band 0xffffffffL) } else { $null })
            nonBlockCjkSymbolsAndPunctuationCount = $(if ($nonBlockRange.Count -ne 0) { [int64]$nonBlockRange[0].value1 } else { $null })
            nonBlockGeneralPunctuationCount = $(if ($nonBlockRange.Count -ne 0) { [int64]$nonBlockRange[0].value2 } else { $null })
            nonBlockCombiningOrVariationCount = $(if ($nonBlockRisk.Count -ne 0) { [int64]$nonBlockRisk[0].value1 } else { $null })
            nonBlockSurrogatePairCount = $(if ($nonBlockRisk.Count -ne 0) { [int64]$nonBlockRisk[0].value2 } else { $null })
            nonBlockLatinExtendedCount = $(if ($nonBlockOther.Count -ne 0) { [int64]$nonBlockOther[0].value1 } else { $null })
            nonBlockOtherBmpCount = $(if ($nonBlockOther.Count -ne 0) { [int64]$nonBlockOther[0].value2 } else { $null })
        }
    }
    $miniMapPaint = @($orderedRecords | Where-Object { $_.event -eq 'startup_draw_minimap_paint_summary' } | Select-Object -First 1)
    $miniMapUpdate = @($orderedRecords | Where-Object { $_.event -eq 'startup_draw_minimap_update_summary' } | Select-Object -First 1)
    if ($miniMapPaint.Count -ne 0 -or $miniMapUpdate.Count -ne 0) {
        $summary.startupMiniMapMetrics = [ordered]@{
            paintTicks = $(if ($miniMapPaint.Count -ne 0) { [int64]$miniMapPaint[0].value1 } else { $null })
            paintMs = $(if ($miniMapPaint.Count -ne 0) { [Math]::Round(($miniMapPaint[0].value1 * 1000.0) / $miniMapPaint[0].frequency, 3) } else { $null })
            paintCount = $(if ($miniMapPaint.Count -ne 0) { [int64]$miniMapPaint[0].value2 } else { $null })
            immediateUpdateCount = $(if ($miniMapUpdate.Count -ne 0) { [int64]$miniMapUpdate[0].value1 } else { $null })
        }
    }
    $firstContent = @($orderedRecords | Where-Object { $_.event -eq 'first_content_painted' } | Select-Object -First 1)
    if ($firstContent.Count -ne 0) {
        $summary.firstContentPainted = $firstContent[0]
        if ($firstContent[0].frequency -eq $LaunchFrequency -and $firstContent[0].qpc -ge $LaunchQpc) {
            $summary.clockCompatible = $true
            $summary.firstContentPaintedMs = [Math]::Round((($firstContent[0].qpc - $LaunchQpc) * 1000.0) / $LaunchFrequency, 3)
        }
    }
    $summary.collected = $true
    return [pscustomobject]$summary
}

function Assert-OwnedProfilePath([string]$ProfilePath, [string]$ExeDirectory, [string]$ProfileName) {
    $resolvedProfile = Get-NormalizedPath $ProfilePath
    $resolvedParent = Get-NormalizedPath (Split-Path -Parent $ProfilePath)
    if ($resolvedParent -ne (Get-NormalizedPath $ExeDirectory) -or
        [IO.Path]::GetFileName($resolvedProfile) -ne $ProfileName.ToUpperInvariant() -or
        -not $ProfileName.StartsWith('startup-probe-', [StringComparison]::Ordinal)) {
        throw "Refusing an unsafe profile path: $ProfilePath"
    }
}

function Remove-OwnedProfile([string]$ProfilePath, [string]$ExeDirectory, [string]$ProfileName) {
    Assert-OwnedProfilePath $ProfilePath $ExeDirectory $ProfileName
    if (Test-Path -LiteralPath $ProfilePath) {
        $profileItem = Get-Item -LiteralPath $ProfilePath -Force
        if (($profileItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Refusing to recurse into a profile reparse point: $ProfilePath"
        }
        [void](Get-SafeDirectoryTreeEntries $ProfilePath)
        try {
            [IO.Directory]::Delete($ProfilePath, $true)
        }
        catch [UnauthorizedAccessException] {
            foreach ($entry in @(Get-SafeDirectoryTreeEntries $ProfilePath | Where-Object { -not $_.IsDirectory })) {
                if (($entry.Attributes -band [IO.FileAttributes]::ReadOnly) -ne 0) {
                    [IO.File]::SetAttributes($entry.FullName, $entry.Attributes -band (-bnot [IO.FileAttributes]::ReadOnly))
                }
            }
            [IO.Directory]::Delete($ProfilePath, $true)
        }
    }
}

function Get-OwnedProcessDepth($Record, $Owned) {
    $depth = 0
    $current = $Record
    while ($Owned.ContainsKey($current.ParentId)) {
        $depth++
        $current = $Owned[$current.ParentId]
    }
    return $depth
}

function Get-ParentFirstProcessOrder {
    param(
        [Parameter(Mandatory = $true)] [object[]]$Ancestry,
        [hashtable]$Owned = $null
    )
    if ($null -eq $Owned) {
        return @($Ancestry | Sort-Object -Property depth, sequence)
    }
    return @($Ancestry | Sort-Object @{ Expression = { Get-OwnedProcessDepth $_ $Owned } }, Id)
}

function Get-TrackedOwnedProcesses($Owned, [object]$CleanupObservation = $null, [AllowNull()] [object]$ContainmentProof = $null) {
    if ($null -ne $CleanupObservation) { Add-StartupTrackedSweepCount $CleanupObservation 'trackedSweepPassCount' }
    try {
        $entries = @(Get-VerifiedProcessEntries $CleanupObservation)
    }
    catch {
        if ($null -ne $CleanupObservation) {
            $enumerationFailureCount = Convert-StartupObservationInt (Get-StartupObservationProperty $CleanupObservation 'processEnumerationFailureCount')
            $enumerationError = Convert-StartupObservationErrorCode (Get-StartupObservationProperty $CleanupObservation 'processEnumerationErrorCode')
            if ($enumerationFailureCount -gt 0) {
                Set-StartupTrackedSweepFailure $CleanupObservation 'enumeration-unavailable' $enumerationError
            }
            else {
                Set-StartupTrackedSweepFailure $CleanupObservation 'exception' 13
            }
        }
        throw
    }
    $records = New-Object Collections.Generic.List[object]
    foreach ($record in @($Owned.Values)) {
        $entry = @($entries | Where-Object { [int]$_.ProcessId -eq [int]$record.Id } | Select-Object -First 1)
        if ($entry.Count -eq 0) { continue }
        if ($null -ne $CleanupObservation) { Add-StartupTrackedSweepCount $CleanupObservation 'trackedSweepIdentityAttemptCount' }
        $identityProbe = $null
        try {
            $identityProbe = [NativeStartupProbe]::QueryProcessIdentity([int]$entry[0].ProcessId, [int]$entry[0].ParentProcessId)
        }
        catch {
            if ($null -ne $CleanupObservation) {
                Add-StartupTrackedSweepCount $CleanupObservation 'trackedSweepIdentityFailureCount'
                Set-StartupTrackedSweepFailure $CleanupObservation 'identity-unavailable' 13
            }
            throw "Could not verify the identity of tracked process $($record.Id) after job cleanup (Win32 13)."
        }
        if (-not $identityProbe.Succeeded -or $null -eq $identityProbe.Identity) {
            if ($null -ne $CleanupObservation) { Add-StartupTrackedSweepCount $CleanupObservation 'trackedSweepIdentityFailureCount' }
            if (Invoke-StartupTrackedIdentityFailure $CleanupObservation ([int]$record.Id) $identityProbe $null `
                    "Could not verify the identity of tracked process $($record.Id) after job cleanup (Win32 $($identityProbe.ErrorCode))." `
                    $true 'post-close-tracked-history' $ContainmentProof) { continue }
        }
        try {
            $current = Convert-ProcessIdentity $identityProbe.Identity
        }
        catch {
            if ($null -ne $CleanupObservation) { Add-StartupTrackedSweepCount $CleanupObservation 'trackedSweepIdentityFailureCount' }
            if (Invoke-StartupTrackedIdentityFailure $CleanupObservation ([int]$record.Id) $identityProbe $null `
                    "Could not verify the identity of tracked process $($record.Id) after job cleanup (Win32 13)." `
                    $true 'post-close-tracked-history' $ContainmentProof) { continue }
        }
        if ($current.Creation -eq $record.Creation -and (Test-SamePath $current.ImagePath $record.ImagePath)) {
            [void]$records.Add($current)
        }
    }
    return $records.ToArray()
}

function Convert-StartupTerminalJobQuery {
    param([AllowNull()] [object]$Candidate)
    $strictEnvelope = Test-StartupStrictJobQueryEnvelope $Candidate
    if (-not $strictEnvelope.valid) {
        return [pscustomobject][ordered]@{
            valid = $false
            errorCode = [int]$strictEnvelope.errorCode
            reason = [string]$strictEnvelope.reason
            memberCount = $null
            observation = New-StartupJobQueryObservation
        }
    }
    $observation = Convert-StartupJobQueryObservation $Candidate
    $memberCount = [int]$strictEnvelope.processIds.Length
    if (-not $observation.attempted -or -not $observation.succeeded -or $observation.queryCount -ne 1 -or
        $observation.errorCode -ne 0 -or $observation.assignedProcessCount -ne [UInt64]$memberCount -or
        $observation.listedProcessCount -ne [UInt64]$memberCount) {
        return [pscustomobject][ordered]@{
            valid = $false
            errorCode = 13
            reason = 'normalized-query-equation'
            memberCount = $null
            observation = New-StartupJobQueryObservation
        }
    }
    return [pscustomobject][ordered]@{
        valid = $true
        errorCode = 0
        reason = 'none'
        memberCount = $memberCount
        observation = $observation
    }
}

function Test-StartupContainmentProofV2 {
    param([AllowNull()] [object]$Proof)
    try {
        if ($null -eq $Proof -or $Proof -isnot [Collections.IDictionary]) { return $false }
        $expectedProofFields = @('version', 'mode', 'terminalState', 'terminationAttempted', 'terminationSucceeded',
            'terminationErrorCode', 'terminalJobQuery', 'terminalJobMemberCount', 'jobEmptyProven', 'identityReconciliation')
        if ((@($Proof.Keys | Sort-Object) -join ',') -ne (@($expectedProofFields | Sort-Object) -join ',')) { return $false }
        if ((Get-StartupObservationProperty $Proof 'version') -isnot [int32] -or [int]$Proof.version -ne 2 -or
            $Proof.mode -isnot [string] -or $startupContainmentModes -notcontains [string]$Proof.mode -or
            $Proof.terminalState -isnot [string] -or $startupContainmentTerminalStates -notcontains [string]$Proof.terminalState -or
            $Proof.terminationAttempted -isnot [bool] -or $Proof.terminationSucceeded -isnot [bool] -or
            $Proof.jobEmptyProven -isnot [bool]) { return $false }
        $terminationError = Get-StartupObservationProperty $Proof 'terminationErrorCode'
        if ([bool]$Proof.terminationAttempted) {
            if ([bool]$Proof.terminationSucceeded) {
                if ($null -ne $terminationError) { return $false }
            }
            elseif ($terminationError -isnot [int32] -or [int]$terminationError -le 0) { return $false }
        }
        elseif ([bool]$Proof.terminationSucceeded -or $null -ne $terminationError) { return $false }
        $terminalQuery = Get-StartupObservationProperty $Proof 'terminalJobQuery'
        if ($null -eq $terminalQuery -or $terminalQuery -isnot [Collections.IDictionary]) { return $false }
        $expectedQueryFields = @((New-StartupJobQueryObservation).Keys)
        if ((@($terminalQuery.Keys | Sort-Object) -join ',') -ne (@($expectedQueryFields | Sort-Object) -join ',')) { return $false }
        foreach ($boolField in @('attempted', 'skipped', 'succeeded', 'resized', 'attemptsTruncated')) {
            if ((Get-StartupObservationProperty $terminalQuery $boolField) -isnot [bool]) { return $false }
        }
        foreach ($intField in @('queryCount', 'attemptCount')) {
            $intValue = Get-StartupObservationProperty $terminalQuery $intField
            if ($intValue -isnot [int32] -or [int]$intValue -lt 0 -or [int]$intValue -gt $startupJobQueryMaxAttemptCount) { return $false }
        }
        foreach ($uint64Field in @('capacityBytes', 'requiredBytes', 'returnLengthBytes')) {
            $uint64Value = Get-StartupObservationProperty $terminalQuery $uint64Field
            if ($uint64Value -isnot [UInt64] -or [UInt64]$uint64Value -gt $startupJobQueryMaxBytes) { return $false }
        }
        foreach ($countField in @('assignedProcessCount', 'listedProcessCount')) {
            $countValue = Get-StartupObservationProperty $terminalQuery $countField
            if ($countValue -isnot [UInt64] -or [UInt64]$countValue -gt [UInt64]$startupJobQueryMaxProcessCount) { return $false }
        }
        $queryError = Get-StartupObservationProperty $terminalQuery 'errorCode'
        if ($null -ne $queryError -and $queryError -isnot [int32]) { return $false }
        $normalizedAttempts = $terminalQuery['attempts']
        if ($normalizedAttempts -isnot [Array] -or $normalizedAttempts.Length -gt $startupJobQueryMaxAttemptCount -or
            $normalizedAttempts.Length -ne [int]$terminalQuery.attemptCount) { return $false }
        $previousDataCapacity = $null
        $anyAttemptResized = $false
        $lastAttempt = $null
        for ($attemptIndex = 0; $attemptIndex -lt $normalizedAttempts.Length; $attemptIndex++) {
            $normalizedAttempt = $normalizedAttempts[$attemptIndex]
            if ($normalizedAttempt -isnot [Collections.IDictionary]) { return $false }
            $expectedAttemptFields = @('attempted', 'attemptNumber', 'succeeded', 'errorCode', 'capacityBytes',
                'requiredBytes', 'returnLengthBytes', 'assignedProcessCount', 'listedProcessCount', 'resized')
            if ((@($normalizedAttempt.Keys | Sort-Object) -join ',') -ne (@($expectedAttemptFields | Sort-Object) -join ',')) { return $false }
            if ($normalizedAttempt.attempted -isnot [bool] -or $normalizedAttempt.succeeded -isnot [bool] -or
                $normalizedAttempt.resized -isnot [bool] -or $normalizedAttempt.errorCode -isnot [int32] -or
                $normalizedAttempt.attemptNumber -isnot [int32] -or [int]$normalizedAttempt.attemptNumber -ne ($attemptIndex + 1) -or
                -not [bool]$normalizedAttempt.attempted -or [int]$normalizedAttempt.errorCode -lt 0) { return $false }
            foreach ($attemptUInt64Field in @('capacityBytes', 'requiredBytes', 'returnLengthBytes', 'assignedProcessCount', 'listedProcessCount')) {
                $attemptUInt64Value = Get-StartupObservationProperty $normalizedAttempt $attemptUInt64Field
                if ($attemptUInt64Value -isnot [UInt64]) { return $false }
            }
            if ($normalizedAttempt.capacityBytes -gt $startupJobQueryMaxBytes -or
                $normalizedAttempt.requiredBytes -gt $startupJobQueryMaxBytes -or
                $normalizedAttempt.returnLengthBytes -gt $startupJobQueryMaxBytes -or
                $normalizedAttempt.assignedProcessCount -gt [UInt64]$startupJobQueryMaxProcessCount -or
                $normalizedAttempt.listedProcessCount -gt [UInt64]$startupJobQueryMaxProcessCount -or
                $normalizedAttempt.listedProcessCount -gt $normalizedAttempt.assignedProcessCount) { return $false }
            if ($normalizedAttempt.capacityBytes -eq [UInt64]0) {
                if ($attemptIndex -ne 0 -or $normalizedAttempt.assignedProcessCount -ne [UInt64]0 -or
                    $normalizedAttempt.listedProcessCount -ne [UInt64]0 -or
                    $normalizedAttempt.requiredBytes -ne $normalizedAttempt.returnLengthBytes -or
                    [bool]$normalizedAttempt.resized) { return $false }
            }
            else {
                $minimumCapacity = $startupJobQueryHeaderBytes + [UInt64]([IntPtr]::Size)
                $capacitySlots = ($normalizedAttempt.capacityBytes - $startupJobQueryHeaderBytes) / [UInt64]([IntPtr]::Size)
                if ($normalizedAttempt.capacityBytes -lt $minimumCapacity -or
                    $normalizedAttempt.requiredBytes -ne $normalizedAttempt.capacityBytes -or
                    $normalizedAttempt.listedProcessCount -gt $capacitySlots -or
                    ($null -ne $previousDataCapacity -and $normalizedAttempt.capacityBytes -le [UInt64]$previousDataCapacity)) { return $false }
                $previousDataCapacity = [UInt64]$normalizedAttempt.capacityBytes
            }
            if ([bool]$normalizedAttempt.succeeded) {
                if ($normalizedAttempt.errorCode -ne 0) { return $false }
                if ($normalizedAttempt.returnLengthBytes -eq [UInt64]0) {
                    if ($normalizedAttempt.assignedProcessCount -ne [UInt64]0 -or
                        $normalizedAttempt.listedProcessCount -ne [UInt64]0) { return $false }
                }
                else {
                    $minimumReturn = $startupJobQueryHeaderBytes +
                        ($normalizedAttempt.listedProcessCount * [UInt64]([IntPtr]::Size))
                    if ($normalizedAttempt.returnLengthBytes -lt $minimumReturn -or
                        $normalizedAttempt.returnLengthBytes -gt $normalizedAttempt.capacityBytes) { return $false }
                }
            }
            elseif ($normalizedAttempt.errorCode -le 0) { return $false }
            $isFinalAttempt = $attemptIndex -eq ($normalizedAttempts.Length - 1)
            $isRetryableFailure = -not [bool]$normalizedAttempt.succeeded -and
                $startupJobQueryRetryableErrorCodes -contains [int]$normalizedAttempt.errorCode
            if (-not $isFinalAttempt) {
                if ($normalizedAttempt.capacityBytes -eq [UInt64]0) {
                    if ($attemptIndex -ne 0 -or (-not [bool]$normalizedAttempt.succeeded -and -not $isRetryableFailure)) { return $false }
                }
                elseif ([bool]$normalizedAttempt.succeeded) {
                    if ($normalizedAttempt.listedProcessCount -ge $normalizedAttempt.assignedProcessCount -or
                        -not [bool]$normalizedAttempt.resized) { return $false }
                }
                elseif (-not $isRetryableFailure -or -not [bool]$normalizedAttempt.resized) { return $false }
            }
            elseif ([bool]$normalizedAttempt.succeeded -and [bool]$normalizedAttempt.resized) { return $false }
            if ([bool]$normalizedAttempt.resized) { $anyAttemptResized = $true }
            $lastAttempt = $normalizedAttempt
        }
        if ([bool]$terminalQuery.attempted -ne ([int]$terminalQuery.queryCount -eq 1) -or
            [bool]$terminalQuery.skipped -and [bool]$terminalQuery.attempted -or
            [bool]$terminalQuery.succeeded -and ($queryError -isnot [int32] -or [int]$queryError -ne 0)) { return $false }
        if ([bool]$terminalQuery.attempted) {
            if ($null -eq $lastAttempt -or $terminalQuery.capacityBytes -ne $lastAttempt.capacityBytes -or
                $terminalQuery.requiredBytes -ne $lastAttempt.requiredBytes -or
                $terminalQuery.returnLengthBytes -ne $lastAttempt.returnLengthBytes -or
                $terminalQuery.assignedProcessCount -ne $lastAttempt.assignedProcessCount -or
                $terminalQuery.listedProcessCount -ne $lastAttempt.listedProcessCount -or
                [bool]$terminalQuery.resized -ne $anyAttemptResized -or
                [bool]$terminalQuery.succeeded -ne [bool]$lastAttempt.succeeded -or
                [int]$queryError -ne [int]$lastAttempt.errorCode) { return $false }
        }
        elseif ($terminalQuery.queryCount -ne 0 -or $terminalQuery.attemptCount -ne 0 -or $normalizedAttempts.Length -ne 0 -or
            $null -ne $queryError -or [bool]$terminalQuery.succeeded -or [bool]$terminalQuery.resized) { return $false }
        $memberCount = Get-StartupObservationProperty $Proof 'terminalJobMemberCount'
        if ($null -ne $memberCount -and ($memberCount -isnot [int32] -or [int]$memberCount -lt 0)) { return $false }
        $querySucceeded = (Get-StartupObservationProperty $terminalQuery 'succeeded') -is [bool] -and [bool]$terminalQuery.succeeded
        if ($querySucceeded) {
            if ($memberCount -isnot [int32] -or [int]$memberCount -ne [int]$terminalQuery.listedProcessCount -or
                [UInt64]$terminalQuery.assignedProcessCount -ne [UInt64]$terminalQuery.listedProcessCount -or
                [bool]$Proof.jobEmptyProven -ne ([int]$memberCount -eq 0)) { return $false }
        }
        elseif ($null -ne $memberCount -or [bool]$Proof.jobEmptyProven) { return $false }
        if ([bool]$Proof.jobEmptyProven) {
            if (-not $querySucceeded -or $memberCount -isnot [int32] -or [int]$memberCount -ne 0 -or
                $terminalQuery.assignedProcessCount -ne [UInt64]0 -or $terminalQuery.listedProcessCount -ne [UInt64]0) { return $false }
            $expectedMode = if ([bool]$Proof.terminationAttempted -and [bool]$Proof.terminationSucceeded) {
                'explicit-job-termination'
            }
            elseif (-not [bool]$Proof.terminationAttempted) { 'graceful-job-empty' }
            else { return $false }
            if ([string]$Proof.mode -ne $expectedMode) { return $false }
        }
        elseif ([string]$Proof.mode -ne 'unavailable') { return $false }
        $verifiedState = [string]$Proof.terminalState -in @('verified-graceful-job-empty', 'verified-explicit-job-termination')
        if ($verifiedState) {
            if (-not [bool]$Proof.jobEmptyProven) { return $false }
            if (($Proof.terminalState -eq 'verified-graceful-job-empty') -ne ($Proof.mode -eq 'graceful-job-empty')) { return $false }
            if (($Proof.terminalState -eq 'verified-explicit-job-termination') -ne ($Proof.mode -eq 'explicit-job-termination')) { return $false }
        }
        elseif ([bool]$Proof.jobEmptyProven -and $Proof.terminalState -ne 'rejected-job-close') { return $false }
        if ($Proof.terminalState -eq 'verified-graceful-job-empty' -and
            ([bool]$Proof.terminationAttempted -or [bool]$Proof.terminationSucceeded -or $null -ne $terminationError)) { return $false }
        if ($Proof.terminalState -eq 'verified-explicit-job-termination' -and
            (-not [bool]$Proof.terminationAttempted -or -not [bool]$Proof.terminationSucceeded -or $null -ne $terminationError)) { return $false }
        if ($Proof.terminalState -eq 'rejected-termination-failed' -and
            (-not [bool]$Proof.terminationAttempted -or [bool]$Proof.terminationSucceeded -or
                $terminationError -isnot [int32] -or [int]$terminationError -le 0 -or [bool]$Proof.jobEmptyProven)) { return $false }
        $reconciliation = Get-StartupObservationProperty $Proof 'identityReconciliation'
        if ($null -eq $reconciliation -or $reconciliation -isnot [Collections.IDictionary]) { return $false }
        $expectedReconciliationFields = @('attempted', 'accepted', 'operation', 'observerRole', 'reason')
        if ((@($reconciliation.Keys | Sort-Object) -join ',') -ne (@($expectedReconciliationFields | Sort-Object) -join ',')) { return $false }
        if ($reconciliation.attempted -isnot [bool] -or $reconciliation.accepted -isnot [bool] -or
            $reconciliation.operation -isnot [string] -or $startupIdentityOperations -notcontains [string]$reconciliation.operation -or
            $reconciliation.observerRole -isnot [string] -or $startupIdentityObserverRoles -notcontains [string]$reconciliation.observerRole -or
            $reconciliation.reason -isnot [string] -or $startupIdentityReconciliationReasons -notcontains [string]$reconciliation.reason) { return $false }
        if (-not [bool]$reconciliation.attempted) {
            if ([bool]$reconciliation.accepted -or $reconciliation.operation -ne 'none' -or
                $reconciliation.observerRole -ne 'none' -or $reconciliation.reason -ne 'none') { return $false }
        }
        elseif ([bool]$reconciliation.accepted) {
            if (-not [bool]$Proof.jobEmptyProven -or
                $startupIdentityReconciliationOperations -notcontains [string]$reconciliation.operation -or
                $startupIdentityReconciliationObserverRoles -notcontains [string]$reconciliation.observerRole -or
                $reconciliation.reason -ne 'job-empty-and-allowlisted-post-close-history') { return $false }
        }
        else {
            switch ([string]$reconciliation.reason) {
                'job-empty-not-proven' { if ([bool]$Proof.jobEmptyProven) { return $false } }
                'operation-not-allowlisted' {
                    if ($startupIdentityReconciliationOperations -contains [string]$reconciliation.operation) { return $false }
                }
                'observer-not-allowlisted' {
                    if ($startupIdentityReconciliationObserverRoles -contains [string]$reconciliation.observerRole) { return $false }
                }
                'exact-path-failure' { if ($reconciliation.observerRole -ne 'exact-path') { return $false } }
                default { return $false }
            }
        }
        $proofJson = $Proof | ConvertTo-Json -Depth 10 -Compress
        return $proofJson -notmatch '(?i)processids|\bpid\b|imagepath|commandline|payload|raw'
    }
    catch { return $false }
}

function Convert-StartupTerminationActionEnvelope {
    param([AllowNull()] [object]$Candidate)
    $attempted = Get-StartupObservationProperty $Candidate 'Attempted'
    $succeeded = Get-StartupObservationProperty $Candidate 'Succeeded'
    $errorCode = Get-StartupObservationProperty $Candidate 'ErrorCode'
    $shapeValid = $null -ne $Candidate -and $attempted -is [bool] -and [bool]$attempted -and
        $succeeded -is [bool] -and $errorCode -is [int32]
    $equationValid = $shapeValid -and (([bool]$succeeded -and [int]$errorCode -eq 0) -or
        (-not [bool]$succeeded -and [int]$errorCode -gt 0))
    return [pscustomobject][ordered]@{
        valid = [bool]$equationValid
        succeeded = [bool]($equationValid -and [bool]$succeeded)
        errorCode = if ($equationValid) { [int]$errorCode } else { 13 }
    }
}

function Convert-StartupCloseActionEnvelope {
    param([AllowNull()] [object]$Candidate)
    $succeeded = Get-StartupObservationProperty $Candidate 'Succeeded'
    $errorCode = Get-StartupObservationProperty $Candidate 'ErrorCode'
    $handle = Get-StartupObservationProperty $Candidate 'Handle'
    $shapeValid = $null -ne $Candidate -and $succeeded -is [bool] -and
        $errorCode -is [int32] -and $handle -is [IntPtr]
    $equationValid = $shapeValid -and (([bool]$succeeded -and [int]$errorCode -eq 0 -and
            [IntPtr]$handle -eq [IntPtr]::Zero) -or
        (-not [bool]$succeeded -and [int]$errorCode -gt 0))
    return [pscustomobject][ordered]@{
        valid = [bool]$equationValid
        succeeded = [bool]($equationValid -and [bool]$succeeded)
        errorCode = if ($equationValid) { [int]$errorCode } else { 13 }
        handle = if ($shapeValid) { [IntPtr]$handle } else { [IntPtr]::Zero }
    }
}

function Invoke-StartupJobCleanupStateMachine {
    param(
        [Parameter(Mandatory = $true)] [IntPtr]$Job,
        [Parameter(Mandatory = $true)] [object]$Owned,
        [Parameter(Mandatory = $true)] [object]$CleanupObservation,
        [AllowNull()] [object]$Invokers = $null,
        [int]$TimeoutMs = $closeTimeoutMs
    )
    $proof = New-StartupContainmentProof
    $CleanupObservation.containmentProof = $proof
    $remainingHandle = $Job
    $closeAttempted = $false
    $closeSucceeded = $false
    $graceful = $false
    $outerPollCount = 0
    $queryInvoker = { param([IntPtr]$Handle) [NativeStartupProbe]::QueryJobProcessIds($Handle) }
    $terminateInvoker = { param([IntPtr]$Handle) [NativeStartupProbe]::TerminateKillOnCloseJob($Handle) }
    $closeInvoker = { param([IntPtr]$Handle) [NativeStartupProbe]::CloseKillOnCloseJob($Handle) }
    $gracefulInvoker = {
        return @(Get-LiveOwnedProcesses $Owned $Job $CleanupObservation $CleanupObservation 'graceful-job-member').Count -eq 0
    }
    $delayInvoker = { param([int]$DelayMs) Start-Sleep -Milliseconds $DelayMs }
    $invokerShapeValid = $true
    if ($null -ne $Invokers) {
        foreach ($entry in @(
                @('QueryJob', [ref]$queryInvoker), @('TerminateJob', [ref]$terminateInvoker),
                @('CloseJob', [ref]$closeInvoker), @('ObserveGraceful', [ref]$gracefulInvoker),
                @('Delay', [ref]$delayInvoker))) {
            $candidate = Get-StartupObservationProperty $Invokers $entry[0]
            if ($null -ne $candidate) {
                if ($candidate -isnot [scriptblock]) { $invokerShapeValid = $false }
                else { $entry[1].Value = $candidate }
            }
        }
    }
    if ($TimeoutMs -lt 1) { $TimeoutMs = 1 }
    $stateWatch = [Diagnostics.Stopwatch]::StartNew()
    try {
        if (-not $invokerShapeValid) { throw 'The cleanup state-machine invoker seam is malformed.' }
        if ($Job -eq [IntPtr]::Zero) {
            $proof.terminalState = 'rejected-job-unavailable'
            return [pscustomobject][ordered]@{
                proof = $proof; remainingJobHandle = $remainingHandle; closeAttempted = $false
                closeSucceeded = $false; graceful = $false; outerPollCount = 0
            }
        }
        $requiresTermination = $true
        for ($gracefulPoll = 0; $gracefulPoll -lt $startupContainmentMaxOuterPolls -and
                $stateWatch.ElapsedMilliseconds -lt $TimeoutMs; $gracefulPoll++) {
            try {
                $gracefulValue = & $gracefulInvoker
                if ($gracefulValue -isnot [bool]) { $graceful = $false; break }
                $graceful = [bool]$gracefulValue
            }
            catch { $graceful = $false; break }
            if ($graceful) {
                $terminalCandidate = $null
                $outerPollCount++
                try { $terminalCandidate = & $queryInvoker $Job } catch { $terminalCandidate = $null }
                $terminal = Convert-StartupTerminalJobQuery $terminalCandidate
                $proof.terminalJobQuery = $terminal.observation
                if ($terminal.valid) { $proof.terminalJobMemberCount = [int]$terminal.memberCount }
                if ($terminal.valid -and $terminal.memberCount -eq 0) {
                    $proof.jobEmptyProven = $true
                    $proof.mode = 'graceful-job-empty'
                    $requiresTermination = $false
                    break
                }
                break
            }
            if ($gracefulPoll + 1 -lt $startupContainmentMaxOuterPolls) { & $delayInvoker $pollIntervalMs }
        }
        if ($requiresTermination) {
            $proof.terminationAttempted = $true
            $termination = $null
            try { $termination = & $terminateInvoker $Job } catch { $termination = $null }
            $terminationEnvelope = Convert-StartupTerminationActionEnvelope $termination
            if (-not $terminationEnvelope.valid -or -not $terminationEnvelope.succeeded) {
                $proof.terminationSucceeded = $false
                $proof.terminationErrorCode = if ($terminationEnvelope.errorCode -gt 0) { [int]$terminationEnvelope.errorCode } else { 13 }
                $proof.terminalState = 'rejected-termination-failed'
            }
            else {
                $proof.terminationSucceeded = $true
                $lastTerminalValid = $false
                for ($terminalPoll = 0; $outerPollCount -lt $startupContainmentMaxOuterPolls -and
                        ($terminalPoll -eq 0 -or $stateWatch.ElapsedMilliseconds -lt $TimeoutMs); $terminalPoll++) {
                    $terminalCandidate = $null
                    $outerPollCount++
                    try { $terminalCandidate = & $queryInvoker $Job } catch { $terminalCandidate = $null }
                    $terminal = Convert-StartupTerminalJobQuery $terminalCandidate
                    $proof.terminalJobQuery = $terminal.observation
                    if (-not $terminal.valid) {
                        $proof.terminalJobMemberCount = $null
                        $proof.terminalState = 'rejected-terminal-job-query'
                        break
                    }
                    $lastTerminalValid = $true
                    $proof.terminalJobMemberCount = [int]$terminal.memberCount
                    if ($terminal.memberCount -eq 0) {
                        $proof.jobEmptyProven = $true
                        $proof.mode = 'explicit-job-termination'
                        break
                    }
                    $proof.terminalState = 'rejected-job-members-remain'
                    if ($terminalPoll + 1 -lt $startupContainmentMaxOuterPolls) { & $delayInvoker $pollIntervalMs }
                }
                if (-not $proof.jobEmptyProven -and $proof.terminalState -eq 'not-attempted') {
                    $proof.terminalState = if ($lastTerminalValid) { 'rejected-job-members-remain' } else { 'rejected-terminal-job-query' }
                }
            }
        }
    }
    catch {
        $proof.terminalState = 'exception'
    }
    finally {
        if ($Job -ne [IntPtr]::Zero) {
            $closeAttempted = $true
            $CleanupObservation.jobCloseAttempted = $true
            try {
                $closeResult = & $closeInvoker $Job
                $closeEnvelope = Convert-StartupCloseActionEnvelope $closeResult
                $closeSucceeded = $closeEnvelope.valid -and $closeEnvelope.succeeded
                if ($closeSucceeded) { $remainingHandle = [IntPtr]::Zero }
            }
            catch { $closeSucceeded = $false }
            $CleanupObservation.jobCloseSucceeded = [bool]$closeSucceeded
            if (-not $closeSucceeded) { $proof.terminalState = 'rejected-job-close' }
        }
        if ($proof.jobEmptyProven -and $closeSucceeded) {
            $proof.terminalState = if ($proof.mode -eq 'graceful-job-empty') {
                'verified-graceful-job-empty'
            }
            else { 'verified-explicit-job-termination' }
        }
    }
    return [pscustomobject][ordered]@{
        proof = $proof; remainingJobHandle = $remainingHandle; closeAttempted = $closeAttempted
        closeSucceeded = $closeSucceeded; graceful = [bool]$graceful; outerPollCount = $outerPollCount
    }
}

function Stop-OwnedProcesses($Owned, [IntPtr]$Job = [IntPtr]::Zero, [string]$ExecutablePath = $null, [object]$QueryObservation = $null, [AllowNull()] [object]$StateMachineInvokers = $null) {
    $closeWatch = [Diagnostics.Stopwatch]::StartNew()
    $cleanupObservation = if ($null -ne $QueryObservation) { $QueryObservation } else { New-StartupCleanupObservation }
    $cleanupObservation.attempted = $true
    $cleanupObservation.jobPresent = $Job -ne [IntPtr]::Zero
    $cleanupObservation.jobQuerySkipped = $Job -eq [IntPtr]::Zero
    $cleanupErrors = New-Object Collections.Generic.List[string]
    $tracked = @()
    $pathMatches = @()
    $trackedSweepAttempted = $false
    $trackedSweepVerified = $false
    $finalPathSweepAttempted = $false
    $finalPathSweepVerified = $false
    $state = $null

    try {
        # Graceful close remains best-effort.  The Job state machine below owns
        # the terminal decision and never treats a window/PID observation as a
        # substitute for an empty run-owned Job query.
        $cleanupObservation.gracefulCloseAttempted = $true
        $windows = @([NativeStartupProbe]::GetTopLevelWindows() | Where-Object { $Owned.ContainsKey([int]$_.ProcessId) })
        $windowRecords = foreach ($window in $windows) {
            $record = $Owned[[int]$window.ProcessId]
            [pscustomobject]@{ Window = $window; Depth = Get-OwnedProcessDepth $record $Owned; Id = [int]$record.Id }
        }
        foreach ($windowRecord in @($windowRecords | Sort-Object Depth, Id)) {
            [void][NativeStartupProbe]::RequestClose($windowRecord.Window.Handle)
        }
    }
    catch { Add-StartupCleanupError $cleanupErrors $cleanupObservation $_.Exception.Message }

    $state = Invoke-StartupJobCleanupStateMachine $Job $Owned $cleanupObservation $StateMachineInvokers $closeTimeoutMs
    $proof = $state.proof
    $remainingJobHandle = [IntPtr]$state.remainingJobHandle
    $jobQuerySucceeded = [bool]$proof.jobEmptyProven
    $jobCloseSucceeded = [bool]$state.closeSucceeded
    $cleanupObservation.gracefulCloseSucceeded = [bool]$state.graceful
    $cleanupObservation.jobQueryAttempted = [bool]$proof.terminalJobQuery.attempted
    $cleanupObservation.jobQuerySucceeded = [bool]$jobQuerySucceeded
    $cleanupObservation.jobCloseSucceeded = [bool]$jobCloseSucceeded
    if ($proof.terminalJobQuery.attempted) { Add-StartupJobQueryObservation $cleanupObservation $proof.terminalJobQuery }
    elseif ($Job -eq [IntPtr]::Zero) {
        $skippedQuery = New-StartupJobQueryObservation
        $skippedQuery.skipped = $true
        Add-StartupJobQueryObservation $cleanupObservation $skippedQuery
    }

    if (-not (Test-StartupContainmentProofV2 $proof) -or
        $proof.terminalState -notin @('verified-graceful-job-empty', 'verified-explicit-job-termination')) {
        Add-StartupCleanupError $cleanupErrors $cleanupObservation "Run-owned Job containment proof ended in '$($proof.terminalState)'."
    }
    if (-not $jobCloseSucceeded -and $Job -ne [IntPtr]::Zero) {
        Add-StartupCleanupError $cleanupErrors $cleanupObservation 'Run-owned Job finalization did not close the Job handle.'
    }

    # These observers are secondary consistency checks.  A tracked-history
    # identity gap is reconcilable only through the v2 proof helper; an exact
    # path observer failure always throws and remains terminal.
    $settled = $false
    for ($sweepPoll = 0; $sweepPoll -lt $startupContainmentMaxOuterPolls -and
            ($sweepPoll -eq 0 -or $closeWatch.ElapsedMilliseconds -lt $closeTimeoutMs); $sweepPoll++) {
        try {
            $trackedSweepAttempted = $true
            $trackedSweepVerified = $false
            $tracked = @(Get-TrackedOwnedProcesses $Owned $cleanupObservation $proof)
            $trackedSweepVerified = $true
            if (-not [string]::IsNullOrWhiteSpace($ExecutablePath)) {
                $finalPathSweepAttempted = $true
                $pathMatches = @(Get-ProcessesForImagePath $ExecutablePath $cleanupObservation)
                $finalPathSweepVerified = $true
            }
            if ($tracked.Count -eq 0 -and $pathMatches.Count -eq 0) { $settled = $true; break }
        }
        catch {
            Add-StartupCleanupError $cleanupErrors $cleanupObservation $_.Exception.Message
            break
        }
        if ($sweepPoll + 1 -lt $startupContainmentMaxOuterPolls) { Start-Sleep -Milliseconds $pollIntervalMs }
    }
    $survivorsById = @{}
    foreach ($record in @($tracked + $pathMatches)) { $survivorsById[[int]$record.Id] = $record }
    if (-not $trackedSweepVerified) { Add-StartupCleanupError $cleanupErrors $cleanupObservation 'The post-close tracked-history sweep was not completed.' }
    if (-not $finalPathSweepVerified) { Add-StartupCleanupError $cleanupErrors $cleanupObservation 'The final exact executable-path sweep was not completed.' }
    if (-not $settled -or $survivorsById.Count -ne 0) {
        Add-StartupCleanupError $cleanupErrors $cleanupObservation 'Post-close process observers did not reach a clean terminal state.'
    }
    if ($cleanupErrors.Count -gt 0 -and $proof.terminalState -in @('verified-graceful-job-empty', 'verified-explicit-job-termination')) {
        $proof.terminalState = 'rejected-post-close-observation'
    }
    $cleanupObservation.trackedSweepAttempted = [bool]$trackedSweepAttempted
    $cleanupObservation.trackedSweepVerified = [bool]$trackedSweepVerified
    $cleanupObservation.finalPathSweepAttempted = [bool]$finalPathSweepAttempted
    $cleanupObservation.finalPathSweepVerified = [bool]$finalPathSweepVerified
    $cleanupObservation.survivorCount = [int]$survivorsById.Count
    $cleanupObservation.cleanupErrorCount = [int]$cleanupErrors.Count
    return [pscustomobject][ordered]@{
        survivors = @($survivorsById.Values)
        graceful = [bool]$state.graceful
        jobQuerySucceeded = [bool]$jobQuerySucceeded
        jobCloseSucceeded = [bool]$jobCloseSucceeded
        jobHandle = $remainingJobHandle
        finalPathSweepVerified = [bool]$finalPathSweepVerified
        containmentProof = $proof
        cleanupObservation = $cleanupObservation
        error = if ($cleanupErrors.Count -eq 0) { $null } else { ($cleanupErrors.ToArray() -join ' ') }
    }
}

function Test-StartupProcessCleanupCompletion {
    param(
        [AllowNull()] [object]$Cleanup,
        [IntPtr]$RemainingJobHandle,
        [int]$TerminalErrorCount
    )
    if ($null -eq $Cleanup -or $TerminalErrorCount -ne 0 -or
        $RemainingJobHandle -ne [IntPtr]::Zero) {
        return $false
    }
    $observation = Get-StartupObservationProperty $Cleanup 'cleanupObservation'
    if ($null -eq $observation) { return $false }
    $proof = Get-StartupObservationProperty $Cleanup 'containmentProof'
    if (-not (Test-StartupContainmentProofV2 $proof) -or
        $proof.terminalState -notin @('verified-graceful-job-empty', 'verified-explicit-job-termination') -or
        -not [bool]$proof.jobEmptyProven) {
        return $false
    }
    $observationSurvivorCount = Get-StartupObservationProperty $observation 'survivorCount'
    $observationCleanupErrorCount = Get-StartupObservationProperty $observation 'cleanupErrorCount'
    if ($observationSurvivorCount -isnot [int32] -or $observationSurvivorCount -ne 0 -or
        $observationCleanupErrorCount -isnot [int32] -or $observationCleanupErrorCount -ne 0) {
        return $false
    }
    return @($Cleanup.survivors).Count -eq $observationSurvivorCount -and
        (Get-StartupObservationProperty $Cleanup 'jobQuerySucceeded') -is [bool] -and
        [bool](Get-StartupObservationProperty $Cleanup 'jobQuerySucceeded') -and
        (Get-StartupObservationProperty $observation 'jobQuerySucceeded') -is [bool] -and
        [bool](Get-StartupObservationProperty $observation 'jobQuerySucceeded') -and
        (Get-StartupObservationProperty $Cleanup 'jobCloseSucceeded') -is [bool] -and
        [bool](Get-StartupObservationProperty $Cleanup 'jobCloseSucceeded') -and
        (Get-StartupObservationProperty $observation 'jobCloseSucceeded') -is [bool] -and
        [bool](Get-StartupObservationProperty $observation 'jobCloseSucceeded') -and
        (Get-StartupObservationProperty $observation 'trackedSweepVerified') -is [bool] -and
        [bool](Get-StartupObservationProperty $observation 'trackedSweepVerified') -and
        (Get-StartupObservationProperty $Cleanup 'finalPathSweepVerified') -is [bool] -and
        [bool](Get-StartupObservationProperty $Cleanup 'finalPathSweepVerified') -and
        (Get-StartupObservationProperty $observation 'finalPathSweepVerified') -is [bool] -and
        [bool](Get-StartupObservationProperty $observation 'finalPathSweepVerified')
}

function Invoke-StartupMeasurement([string]$Condition, [int]$Iteration, [string]$ExePath, [string]$DocumentPath, [int]$ExpectedLineCount, [string]$ProfileName, [string]$ProfilePath, [string]$ExeDirectory, [bool]$TakeScreenshot, [string]$ScreenshotPath, [string]$TraceDirectory, [UInt64]$AffinityMask = 0) {
    $owned = @{}
    $job = [IntPtr]::Zero
    $processHandle = [IntPtr]::Zero
    $threadHandle = [IntPtr]::Zero
    $processResumed = $false
    $traceLaunchQpc = [int64]0
    $traceLaunchFrequency = [int64]0
    $result = [ordered]@{
        condition = $Condition; iteration = $Iteration; profileName = $ProfileName
        processApiReturnMs = $null; topLevelHwndMs = $null; visibleMs = $null; dwmFlushMs = $null
        captionReadyMs = $null; inputIdleMs = $null; inputIdleReached = $false; inputIdleError = $null
        documentReadyMs = $null; verticalScrollMaximum = $null
        affinity = [ordered]@{
            requestedMask = [UInt64]$AffinityMask; processMask = $null; systemMask = $null
            opened = $false; setSucceeded = $false; readBackSucceeded = $false; verified = $false; descendantsVerified = $false; errorCode = $null
            historicalOwnedCount = [int]0; currentLiveCount = [int]0; expiredHistoricalCount = [int]0
            failureType = 'none'; failureErrorCode = $null; liveSetSource = 'not-attempted'
        }
        startupTrace = $null
        startupDiagnostics = New-StartupDiagnosticState
        launchJobQueryObservation = New-StartupJobQueryObservation
        cleanupObservation = $null
        containmentProof = New-StartupContainmentProof
        screenshotPath = $null; success = $false; error = $null
        processCleanupVerified = $false; profileCleanupVerified = $false; cleanupVerified = $false
        jobQuerySucceeded = $false; jobCloseSucceeded = $false; finalPathSweepVerified = $false; containmentVerified = $false
        survivors = @()
    }
    # Launch containment and cleanup share one explicit additive identity-race
    # object, so a launch-time Job query cannot lose its diagnostic before the
    # later cleanup report is serialized.
    $jobIdentityObservation = New-StartupJobIdentityObservation
    $result.launchJobQueryObservation['jobIdentityObservation'] = $jobIdentityObservation
    try {
        # A missing or malformed executable-side sidecar makes Sakura resolve the
        # profile through the user's roaming configuration.  Refuse the launch
        # before Process.Start rather than relying on the caller to have bundled it.
        [void](Assert-StartupProfileSidecar $ExePath)
        if (-not (Test-SamePath (Split-Path -Parent $ExePath) $ExeDirectory)) {
            throw 'The executable and owned artifact bundle directory do not match.'
        }
        $arguments = '-PROF="{0}" "{1}"' -f $ProfileName, $DocumentPath
        $startInfo = New-Object Diagnostics.ProcessStartInfo
        $startInfo.FileName = $ExePath
        $startInfo.Arguments = $arguments
        $startInfo.UseShellExecute = $false
        $startInfo.WorkingDirectory = [IO.Path]::GetFullPath($ExeDirectory)
        $watch = [Diagnostics.Stopwatch]::StartNew()
        $traceLaunchQpc = [Diagnostics.Stopwatch]::GetTimestamp()
        $traceLaunchFrequency = [Diagnostics.Stopwatch]::Frequency
        if (-not [string]::IsNullOrWhiteSpace($TraceDirectory)) {
            $startInfo.EnvironmentVariables['SAKURA_STARTUP_TRACE_DIR'] = $TraceDirectory
        }
        else {
            [void]$startInfo.EnvironmentVariables.Remove('SAKURA_STARTUP_TRACE_DIR')
        }
        $jobResult = [NativeStartupProbe]::CreateKillOnCloseJob()
        if (-not $jobResult.Succeeded -or $jobResult.Handle -eq [IntPtr]::Zero) {
            throw "Could not create the run-owned kill-on-close job (Win32 $($jobResult.ErrorCode))."
        }
        $job = $jobResult.Handle
        $traceEnvironmentValue = if ([string]::IsNullOrWhiteSpace($TraceDirectory)) { '' } else { $TraceDirectory }
        $environmentOverrides = [string[]]@('SAKURA_STARTUP_TRACE_DIR=' + $traceEnvironmentValue)
        $launchCommandLine = '"{0}" {1}' -f $ExePath, $arguments
        # Create the process suspended so the root is assigned to the run-owned
        # job before Sakura can create its control/editor children.  This closes
        # the Process.Start/AssignProcessToJobObject escape window.
        $suspended = [NativeStartupProbe]::CreateSuspendedProcess($ExePath, $launchCommandLine, $startInfo.WorkingDirectory, $environmentOverrides)
        if (-not $suspended.Succeeded -or $suspended.ProcessHandle -eq [IntPtr]::Zero -or $suspended.ThreadHandle -eq [IntPtr]::Zero -or $suspended.ProcessId -le 0) {
            throw "Could not create the suspended run-owned process (Win32 $($suspended.ErrorCode))."
        }
        $processHandle = $suspended.ProcessHandle
        $threadHandle = $suspended.ThreadHandle
        $startedProcessId = [int]$suspended.ProcessId
        $result.startupDiagnostics.rootProcessId = $startedProcessId
        $snapshot = Get-ProcessSnapshot @{} ([int[]]@($startedProcessId))
        if (-not $snapshot.ContainsKey($startedProcessId)) { throw "Started process $startedProcessId was not observable." }
        $seed = $snapshot[$startedProcessId]
        if (-not (Test-SamePath $seed.ImagePath $ExePath)) { throw 'Started process image path did not match SakuraExe.' }
        $owned[$seed.Id] = $seed
        $assigned = [NativeStartupProbe]::AssignProcessToKillOnCloseJob($job, $startedProcessId)
        if (-not $assigned.Succeeded) {
            throw "Could not assign the started process to the run-owned job (Win32 $($assigned.ErrorCode))."
        }
        $jobMembers = @(Get-JobProcessRecords $job $owned $result.launchJobQueryObservation $null $null $jobIdentityObservation)
        if (@($jobMembers | Where-Object { [int]$_.Id -eq [int]$startedProcessId }).Count -ne 1) {
            throw 'The started process was not present in the verified run-owned job membership.'
        }
        $result.jobQuerySucceeded = $true
        $result.containmentVerified = $true
        if ($AffinityMask -ne 0) {
            # The primary thread is still suspended.  Apply and read back the
            # requested mask through the exact process handle before any user
            # code can run and create a control/editor child.  Children then
            # inherit the verified root affinity; setting it by PID after
            # ResumeThread would leave an unbounded pre-affinity spawn window.
            $result.affinity = Set-ProcessAffinityHandleVerified $processHandle $startedProcessId $AffinityMask
        }
        $resumed = [NativeStartupProbe]::ResumeSuspendedProcess($threadHandle)
        if (-not $resumed.Succeeded) { throw "Could not resume the job-contained process (Win32 $($resumed.ErrorCode))." }
        $processResumed = $true
        $result.processApiReturnMs = [Math]::Round($watch.Elapsed.TotalMilliseconds, 3)
        $targetCaption = [IO.Path]::GetFileName($DocumentPath)
        $selection = @{ Window = $null }
        $editorIdentified = Wait-WithPoll {
            [void](Update-StartupDiagnosticCheckpoints $result.startupDiagnostics $owned $job $watch.Elapsed.TotalMilliseconds -RootProcessHandle $processHandle)
            foreach ($window in @([NativeStartupProbe]::GetTopLevelWindows())) {
                if (-not $window.ClassName.StartsWith('TextEditorWindow', [StringComparison]::Ordinal)) { continue }
                $observedMs = [Math]::Round($watch.Elapsed.TotalMilliseconds, 3)
                if (-not $owned.ContainsKey([int]$window.ProcessId)) {
                    $candidateSnapshot = Get-ProcessSnapshot $owned
                    Update-OwnedProcesses $owned $candidateSnapshot
                    if (-not $owned.ContainsKey([int]$window.ProcessId)) { continue }
                }
                if ($null -eq $result.topLevelHwndMs) { $result.topLevelHwndMs = $observedMs }
                $selection.Window = $window
                return $true
            }
            return $false
        } $startupTimeoutMs
        if (-not $editorIdentified) {
            [void](Add-StartupDiagnosticCheckpoint $result.startupDiagnostics 'timeout' $owned $job $watch.Elapsed.TotalMilliseconds -RootProcessHandle $processHandle -Force -FailureStage 'window-discovery' -FailureType 'timeout')
            throw 'Timed out waiting for a run-owned TextEditorWindow.'
        }
        $selectedWindow = $selection.Window
        if ($null -eq $selectedWindow) { throw 'The selected editor window was not retained after discovery.' }
        $inputSnapshot = Get-ProcessSnapshot $owned
        $windowPid = [int]$selectedWindow.ProcessId
        if (-not $owned.ContainsKey($windowPid) -or -not (Test-ProcessIdentity $owned[$windowPid] $inputSnapshot)) {
            throw 'The selected editor process exited before readiness observation.'
        }
        $inputProcess = $null
        try {
            $inputProcess = [Diagnostics.Process]::GetProcessById($windowPid)
            $readiness = [ordered]@{
                captionReadyMs = $null
                inputIdleMs = $null
                inputIdleReached = $false
                documentReadyMs = $null
                verticalScrollMaximum = -1
            }
            $allMilestonesObserved = $false
            while ($watch.ElapsedMilliseconds -lt $startupTimeoutMs) {
                [void](Update-StartupDiagnosticCheckpoints $result.startupDiagnostics $owned $job $watch.Elapsed.TotalMilliseconds -RootProcessHandle $processHandle)
                $currentWindow = @([NativeStartupProbe]::GetTopLevelWindows() | Where-Object {
                    $_.Handle -eq $selectedWindow.Handle -and [int]$_.ProcessId -eq $windowPid -and
                    $_.ClassName.StartsWith('TextEditorWindow', [StringComparison]::Ordinal)
                } | Select-Object -First 1)
                if ($currentWindow.Count -eq 0) {
                    throw 'The selected editor window disappeared before all startup milestones were observed.'
                }
                $selectedWindow = $currentWindow[0]
                $observedMs = [Math]::Round($watch.Elapsed.TotalMilliseconds, 3)
                if ($selectedWindow.Visible -and $null -eq $result.visibleMs) {
                    $result.visibleMs = $observedMs
                }

                $captionReady = $selectedWindow.Visible -and
                    ([string]$selectedWindow.Caption).IndexOf($targetCaption, [StringComparison]::OrdinalIgnoreCase) -ge 0
                $inputIdleReady = $false
                if (-not $readiness.inputIdleReached -and $null -eq $result.inputIdleError) {
                    try {
                        # A zero timeout observes this pass only. Keep the Process instance
                        # for the whole loop so polling does not repeatedly open a handle.
                        $inputIdleReady = [bool]$inputProcess.WaitForInputIdle(0)
                    }
                    catch {
                        # Input-idle is diagnostic-only.  Record one bounded typed failure,
                        # stop probing it, and continue toward the external readiness gate.
                        $result.inputIdleError = 'WaitForInputIdleUnavailable'
                    }
                }

                # Sakura's caption can become ready while a large document is still being laid out.
                # The editor's vertical scrollbar range is derived from the completed layout line
                # count, so require it to cover every physical input line before declaring the
                # document ready. The checked-in benchmark sample intentionally exceeds the
                # scrollbar's initial 30-line placeholder range.
                $maximum = [NativeStartupProbe]::GetVerticalScrollMaximum($selectedWindow.Handle)
                $allMilestonesObserved = Update-StartupReadinessState $readiness $observedMs $captionReady $inputIdleReady $maximum $ExpectedLineCount

                # DwmFlush can block for tens of seconds on an otherwise ready window.
                # Keep it as an explicit diagnostic; startup success never depends on it.
                if ($MeasureDwmFlush -and $selectedWindow.Visible -and $null -eq $result.dwmFlushMs) {
                    $flush = [NativeStartupProbe]::DwmFlush()
                    if ($flush -ne 0) { throw "DwmFlush failed with HRESULT 0x{0:X8}." -f $flush }
                    $result.dwmFlushMs = [Math]::Round($watch.Elapsed.TotalMilliseconds, 3)
                }
                if ($allMilestonesObserved) { break }
                Start-Sleep -Milliseconds $pollIntervalMs
            }
            $result.captionReadyMs = $readiness.captionReadyMs
            $result.inputIdleMs = $readiness.inputIdleMs
            $result.inputIdleReached = $readiness.inputIdleReached
            $result.documentReadyMs = $readiness.documentReadyMs
            $result.verticalScrollMaximum = $readiness.verticalScrollMaximum
            if (-not $allMilestonesObserved) {
                $missingMilestones = Get-StartupReadinessMissingMilestones $readiness $ExpectedLineCount
                if (-not $readiness.inputIdleReached -and $null -eq $result.inputIdleError) {
                    $result.inputIdleError = 'WaitForInputIdleNotObserved'
                }
                [void](Add-StartupDiagnosticCheckpoint $result.startupDiagnostics 'timeout' $owned $job $watch.Elapsed.TotalMilliseconds -RootProcessHandle $processHandle -Force -FailureStage 'readiness' -FailureType 'timeout')
                throw ('Timed out waiting for startup milestones: {0}.' -f ($missingMilestones -join '; '))
            }
        }
        finally {
            if ($null -ne $inputProcess) { $inputProcess.Dispose() }
        }
        if ($AffinityMask -ne 0) {
            $historicalAffinityCount = Add-StartupSaturatingCount @($owned.Values).Count 0 $startupObservationMaxCount
            [void](Set-StartupAffinityLiveSetMetadata $result.affinity $historicalAffinityCount 0 'unavailable')
            try {
                $finalSnapshot = Get-ProcessSnapshot $owned
                Update-OwnedProcesses $owned $finalSnapshot
                $historicalAffinityCount = Add-StartupSaturatingCount @($owned.Values).Count 0 $startupObservationMaxCount
                $snapshotLiveRecords = @($owned.Values | Where-Object { Test-ProcessIdentity $_ $finalSnapshot })
                [void](Set-StartupAffinityLiveSetMetadata $result.affinity $historicalAffinityCount $snapshotLiveRecords.Count 'process-snapshot')
                $currentRecords = @(Get-TrackedOwnedProcesses $owned)
                [void](Set-StartupAffinityLiveSetMetadata $result.affinity $historicalAffinityCount $currentRecords.Count 'tracked-sweep')
                try {
                    $affinityVerificationPlan = @(Get-StartupAffinityVerificationPlan $owned $currentRecords)
                }
                catch {
                    $result.affinity.failureType = 'identity'
                    $result.affinity.failureErrorCode = 13
                    throw
                }
                foreach ($record in $affinityVerificationPlan) {
                    [void](Read-ProcessAffinityVerified -ProcessId ([int]$record.Id) -Mask $AffinityMask)
                }
                $result.affinity.descendantsVerified = $true
            }
            catch {
                $result.affinity.descendantsVerified = $false
                $result.affinity.verified = $false
                if ([string]$result.affinity.failureType -eq 'none') {
                    $result.affinity.failureType = 'verification'
                    $result.affinity.failureErrorCode = 13
                }
                throw
            }
        }
        if ($TakeScreenshot -and $null -ne $selectedWindow) {
            if ([NativeStartupProbe]::SaveWindowPng($selectedWindow.Handle, $ScreenshotPath)) { $result.screenshotPath = $ScreenshotPath }
            else { throw 'Screen capture failed for the selected Sakura window.' }
        }
        $result.success = $true
    }
    catch { $result.error = $_.Exception.Message }
    finally {
        # Each owned native handle has an independent close branch.  A failure
        # closing one handle must not skip the job containment check or the
        # remaining handle cleanup.
        $cleanupErrors = New-Object Collections.Generic.List[string]
        if ($processHandle -ne [IntPtr]::Zero) {
            try {
                # Any failure before ResumeSuspendedProcess leaves the process
                # suspended.  This exact handle cannot target a reused PID.
                if (-not $processResumed) {
                    $terminated = [NativeStartupProbe]::TerminateProcessHandle($processHandle)
                    if (-not $terminated.Succeeded -and $terminated.Existed) {
                        [void]$cleanupErrors.Add("Could not terminate the unresumed run-owned process (Win32 $($terminated.ErrorCode)).")
                    }
                }
            }
            catch { [void]$cleanupErrors.Add("Could not terminate the unresumed run-owned process: $($_.Exception.Message)") }
            try {
                if (-not [NativeStartupProbe]::CloseNativeHandle($processHandle)) {
                    [void]$cleanupErrors.Add('Could not close the run-owned process handle.')
                }
                else { $processHandle = [IntPtr]::Zero }
            }
            catch { [void]$cleanupErrors.Add("Could not close the run-owned process handle: $($_.Exception.Message)") }
        }
        if ($threadHandle -ne [IntPtr]::Zero) {
            try {
                if (-not [NativeStartupProbe]::CloseNativeHandle($threadHandle)) {
                    [void]$cleanupErrors.Add('Could not close the run-owned thread handle.')
                }
                else { $threadHandle = [IntPtr]::Zero }
            }
            catch { [void]$cleanupErrors.Add("Could not close the run-owned thread handle: $($_.Exception.Message)") }
        }

        $cleanup = $null
        $cleanupObservation = New-StartupCleanupObservation
        $cleanupObservation.jobIdentityObservation = $jobIdentityObservation
        $result.cleanupObservation = $cleanupObservation
        $joinedCleanupErrorIncluded = $false
        try { $cleanup = Stop-OwnedProcesses $owned $job $ExePath $cleanupObservation }
        catch {
            $result.cleanupObservation = $cleanupObservation
            [void]$cleanupErrors.Add("Owned process cleanup failed: $($_.Exception.Message)")
        }
        if ($null -ne $cleanup) {
            $survivors = @($cleanup.survivors)
            $result.survivors = @($survivors | ForEach-Object { [ordered]@{ pid = $_.Id; creation = $_.Creation; imagePath = $_.ImagePath; parentPid = $_.ParentId } })
            $result.jobQuerySucceeded = [bool]$cleanup.jobQuerySucceeded
            $result.jobCloseSucceeded = [bool]$cleanup.jobCloseSucceeded
            $result.finalPathSweepVerified = [bool]$cleanup.finalPathSweepVerified
            if ($cleanup.PSObject.Properties['jobHandle']) { $job = [IntPtr]$cleanup.jobHandle }
            else { $job = [IntPtr]::Zero }
            if ($cleanup.PSObject.Properties['cleanupObservation']) { $result.cleanupObservation = $cleanup.cleanupObservation }
            if ($cleanup.PSObject.Properties['containmentProof']) { $result.containmentProof = $cleanup.containmentProof }
            if (-not [string]::IsNullOrWhiteSpace([string]$cleanup.error)) {
                [void]$cleanupErrors.Add([string]$cleanup.error)
                $joinedCleanupErrorIncluded = $true
            }
        }
        if ($job -ne [IntPtr]::Zero) {
            try {
                $result.cleanupObservation.jobCloseAttempted = $true
                $remainingJobClose = [NativeStartupProbe]::CloseKillOnCloseJob($job)
                if ($remainingJobClose.Succeeded) {
                    $job = [IntPtr]::Zero
                    $result.cleanupObservation.jobCloseSucceeded = $true
                }
                else {
                    $result.cleanupObservation.jobCloseSucceeded = $false
                    [void]$cleanupErrors.Add("Could not close the remaining run-owned job (Win32 $($remainingJobClose.ErrorCode)).")
                }
            }
            catch {
                $result.cleanupObservation.jobCloseSucceeded = $false
                [void]$cleanupErrors.Add("Could not close the remaining run-owned job: $($_.Exception.Message)")
            }
        }
        $result.cleanupObservation.survivorCount = [int]@($result.survivors).Count
        $result.cleanupObservation.cleanupErrorCount = Get-StartupCleanupErrorCount $result.cleanupObservation ([Int64]$cleanupErrors.Count) $joinedCleanupErrorIncluded
        $result.processCleanupVerified = Test-StartupProcessCleanupCompletion `
            $cleanup $job $cleanupErrors.Count
        if (-not $result.processCleanupVerified) {
            $result.success = $false
            $cleanupError = if ($cleanupErrors.Count -eq 0) { 'Owned Sakura processes survived or containment verification failed.' } else { $cleanupErrors.ToArray() -join ' ' }
            if ([string]::IsNullOrEmpty([string]$result.error)) { $result.error = "Process cleanup failed: $cleanupError" }
            else { $result.error = "$($result.error) Process cleanup failed: $cleanupError" }
        }
    }
    if ($result.processCleanupVerified -and -not [string]::IsNullOrWhiteSpace($TraceDirectory)) {
        try {
            $result.startupTrace = Get-StartupTraceSummary $TraceDirectory $traceLaunchQpc $traceLaunchFrequency
        }
        catch {
            # A malformed optional trace must be visible in report.json, but it must not
            # retroactively change the external process/window timing result.
            $result.startupTrace = [pscustomobject][ordered]@{
                enabled = $true; collected = $false; directory = $TraceDirectory
                error = "Startup trace collection failed: $($_.Exception.Message)"
            }
        }
    }
    elseif ([string]::IsNullOrWhiteSpace($TraceDirectory)) {
        $result.startupTrace = [pscustomobject][ordered]@{
            enabled = $false; collected = $false
        }
    }
    else {
        $result.startupTrace = [pscustomobject][ordered]@{
            enabled = $true; collected = $false; directory = $TraceDirectory
            error = 'Startup trace was not read because run-owned processes survived cleanup.'
        }
    }
    return [pscustomobject]$result
}

function Invoke-SelfTest {
    if ((Get-Statistics ([double[]](1, 3, 2))).medianMs -ne 2) { throw 'Odd median self-test failed.' }
    if ((Get-Statistics ([double[]](1, 4, 2, 3))).medianMs -ne 2.5) { throw 'Even median self-test failed.' }
    $statistics = Get-Statistics ([double[]](10, 20, 30))
    if ($statistics.meanMs -ne 20 -or $statistics.minMs -ne 10 -or $statistics.maxMs -ne 30 -or $statistics.p95Ms -ne 30) { throw 'Statistics self-test failed.' }
    $readinessState = [ordered]@{
        captionReadyMs = $null
        inputIdleMs = $null
        inputIdleReached = $false
        documentReadyMs = $null
        verticalScrollMaximum = -1
    }
    if (Update-StartupReadinessState $readinessState 10 $false $true 25 100) {
        throw 'Readiness state self-test completed before every milestone.'
    }
    if (Update-StartupReadinessState $readinessState 20 $false $false 100 100) {
        throw 'Readiness state self-test coupled document readiness to caption readiness.'
    }
    $missingReadiness = @(Get-StartupReadinessMissingMilestones $readinessState 100)
    if ($readinessState.inputIdleMs -ne 10 -or $readinessState.documentReadyMs -ne 20 -or $readinessState.verticalScrollMaximum -ne 100 -or $missingReadiness.Count -ne 1 -or $missingReadiness[0] -ne 'visible document caption') {
        throw 'Readiness state self-test did not retain independently observed milestones.'
    }
    if (-not (Update-StartupReadinessState $readinessState 30 $true $false 80 100)) {
        throw 'Readiness state self-test did not complete after the final independent milestone.'
    }
    if ($readinessState.captionReadyMs -ne 30 -or $readinessState.inputIdleMs -ne 10 -or $readinessState.documentReadyMs -ne 20 -or $readinessState.verticalScrollMaximum -ne 100) {
        throw 'Readiness state self-test overwrote first-observed milestone values.'
    }
    $optionalInputIdleState = [ordered]@{
        captionReadyMs = $null
        inputIdleMs = $null
        inputIdleReached = $false
        documentReadyMs = $null
        verticalScrollMaximum = -1
    }
    if (-not (Update-StartupReadinessState $optionalInputIdleState 40 $true $false 100 100) -or
        $optionalInputIdleState.inputIdleReached -or $null -ne $optionalInputIdleState.inputIdleMs -or
        @(Get-StartupReadinessMissingMilestones $optionalInputIdleState 100).Count -ne 0) {
        throw 'Readiness state self-test did not keep input idle diagnostic-only.'
    }
    $readinessInputIdleOptionalSelfTestVerified = $true
    $diagnosticState = New-StartupDiagnosticState 1234
    $diagnosticCheckpoints = @($diagnosticState.processTreeSnapshots)
    if ($diagnosticCheckpoints.Count -ne 4 -or
        ($diagnosticCheckpoints | ForEach-Object { $_.checkpoint }) -join ',' -ne '0.5s,2s,10s,timeout' -or
        $diagnosticCheckpoints[0].targetMs -ne 500 -or $diagnosticCheckpoints[1].targetMs -ne 2000 -or
        $diagnosticCheckpoints[2].targetMs -ne 10000 -or $diagnosticCheckpoints[3].targetMs -ne 30000) {
        throw 'Startup process diagnostic checkpoint self-test failed.'
    }
    foreach ($terminalStatus in @('unavailable', 'observed')) {
        $terminalState = New-StartupDiagnosticState 1234
        $terminalSnapshot = $terminalState.processTreeSnapshots[0]
        $terminalSnapshot.status = $terminalStatus
        $terminalSnapshot.observed = $false
        $terminalSnapshot.elapsedMs = 123
        $terminalSnapshot.failureStage = 'terminal-sentinel'
        $terminalSnapshot.failureType = 'terminal-sentinel'
        [void](Update-StartupDiagnosticCheckpoints $terminalState @{} ([IntPtr]::Zero) 600)
        if ($terminalSnapshot.status -ne $terminalStatus -or
            $terminalSnapshot.elapsedMs -ne 123 -or
            $terminalSnapshot.failureStage -ne 'terminal-sentinel' -or
            $terminalSnapshot.failureType -ne 'terminal-sentinel') {
            throw "Startup diagnostic terminal checkpoint self-test failed for status '$terminalStatus'."
        }
    }
    $diagnosticRecord = Convert-StartupDiagnosticProcessMetadata ([pscustomobject]@{
            Id = 1234; ParentId = 12; Creation = [long]987654321; ImagePath = 'C:\Windows\sakura.exe'
        }) $true
    $diagnosticJson = $diagnosticRecord | ConvertTo-Json -Compress
    if ($diagnosticRecord.pid -ne 1234 -or $diagnosticRecord.ppid -ne 12 -or
        $diagnosticRecord.imageName -ne 'sakura.exe' -or $diagnosticRecord.creationTime -ne 987654321 -or
        -not $diagnosticRecord.jobMember -or $diagnosticJson -match '(?i)path|commandline|caption|text') {
        throw 'Payload-free startup process diagnostic metadata self-test failed.'
    }
    $windowClassification = Get-StartupDiagnosticWindowCount @{ 1234 = $true } @(
        [pscustomobject]@{ ProcessId = 1234; ClassName = 'TextEditorWindowSelfTest' }
        [pscustomobject]@{ ProcessId = 1234; ClassName = '#32770' }
        [pscustomobject]@{ ProcessId = 1234; ClassName = 'OtherWindowSelfTest' }
        [pscustomobject]@{ ProcessId = 4321; ClassName = 'TextEditorWindowIgnored' }
    )
    if ($windowClassification.count -ne 3 -or $windowClassification.capped -or
        $windowClassification.editorWindowCount -ne 1 -or
        $windowClassification.dialogWindowCount -ne 1 -or
        $windowClassification.otherWindowCount -ne 1 -or
        ($windowClassification.editorWindowCount + $windowClassification.dialogWindowCount + $windowClassification.otherWindowCount) -ne $windowClassification.count) {
        throw 'Owned top-level window classification self-test failed.'
    }
    $windowClassificationVerified = $true
    $diagnosticSchemaVerified = $true
    $diagnosticTreesEmpty = @($diagnosticCheckpoints | Where-Object { @($_.processTree).Count -ne 0 }).Count -eq 0
    $diagnosticBoundsVerified = [bool]($diagnosticCheckpoints.Count -eq 4 -and
        $diagnosticTreesEmpty -and
        $diagnosticState.processExitObservation.observed -eq $false)
    $base = [IO.Path]::GetFullPath((Join-Path $env:TEMP 'startup-probe-selftest'))
    Assert-OwnedProfilePath (Join-Path $base 'startup-probe-unit') $base 'startup-probe-unit'
    $rejected = $false
    try { Assert-OwnedProfilePath $base $base 'startup-probe-unit' } catch { $rejected = $true }
    if (-not $rejected) { throw 'Unsafe profile path self-test failed.' }
    $bundleRoot = [IO.Path]::GetFullPath((Join-Path $env:TEMP ('startup-probe-bundle-selftest-root-' + [Guid]::NewGuid().ToString('N'))))
    $bundleName = 'startup-probe-bundle-selftest'
    $bundle = $null
    $bundleVerification = $null
    $bundleCleanupVerified = $false
    try {
        [IO.Directory]::CreateDirectory($bundleRoot) | Out-Null
        $bundle = New-StartupArtifactBundle $PSCommandPath $bundleRoot $bundleName
        $bundleVerification = Assert-StartupArtifactBundleUnchanged $bundle
        if (-not $bundleVerification.sourceUnchanged -or -not $bundleVerification.copiedUnchanged -or
            -not $bundleVerification.sidecarVerified -or $bundle.sidecar.multiUser -ne 0) {
            throw 'Artifact bundle identity self-test failed.'
        }
        $badSidecar = Join-Path $bundle.bundlePath $script:StartupProfileSidecarFileName
        [IO.File]::WriteAllText($badSidecar, "[Settings]`r`nMultiUser=1`r`n", (New-Object Text.UnicodeEncoding($false, $true)))
        $sidecarRejected = $false
        try { [void](Assert-StartupProfileSidecar $bundle.executablePath) } catch { $sidecarRejected = $true }
        if (-not $sidecarRejected) { throw 'Nonportable sidecar self-test was accepted.' }
        [IO.File]::WriteAllText($badSidecar, $script:StartupProfileSidecarText, (New-Object Text.UnicodeEncoding($false, $true)))
        [void](Assert-StartupProfileSidecar $bundle.executablePath)
    }
    finally {
        if ($null -ne $bundle) {
            try {
                Remove-StartupArtifactBundle $bundle
                $bundleCleanupVerified = -not (Test-Path -LiteralPath $bundle.bundlePath)
            }
            catch { $bundleCleanupVerified = $false }
        }
        if (Test-Path -LiteralPath $bundleRoot) { [IO.Directory]::Delete($bundleRoot, $true) }
    }
    if ($null -eq $bundleVerification -or -not $bundleCleanupVerified) { throw 'Artifact bundle cleanup self-test failed.' }
    $closureSourceRoot = [IO.Path]::GetFullPath((Join-Path $env:TEMP ('startup-probe-closure-source-' + [Guid]::NewGuid().ToString('N'))))
    $closureBundle = $null
    $closureBundleVerification = $null
    $closureBundleRoot = [IO.Path]::GetFullPath((Join-Path $env:TEMP ('startup-probe-closure-bundle-root-' + [Guid]::NewGuid().ToString('N'))))
    try {
        [IO.Directory]::CreateDirectory($closureSourceRoot) | Out-Null
        [IO.File]::Copy($PSCommandPath, (Join-Path $closureSourceRoot 'sakura.exe'), $false)
        [IO.File]::WriteAllText((Join-Path $closureSourceRoot 'sakura_lang_en_US.dll'), 'self-test-resource', (New-Object Text.UTF8Encoding($false)))
        $closureExecutableInfo = Get-Item -LiteralPath (Join-Path $closureSourceRoot 'sakura.exe') -Force
        $closureResourceInfo = Get-Item -LiteralPath (Join-Path $closureSourceRoot 'sakura_lang_en_US.dll') -Force
        $closureEntries = @(
            [ordered]@{ artifact_id = 'sakura-editor-msvc-x64-debug-product'; destination = 'build/staging/msvc-x64-debug/sakura-editor/sakura.exe'; role = 'editor'; source = 'x64/Debug/sakura.exe'; sha256 = 'sha256:' + (Get-Sha256 (Join-Path $closureSourceRoot 'sakura.exe')); size = [UInt64]$closureExecutableInfo.Length },
            [ordered]@{ artifact_id = 'sakura-language-en-us-resource'; destination = 'build/staging/msvc-x64-debug/sakura-editor/sakura_lang_en_US.dll'; role = 'language-en-us'; source = 'x64/Debug/sakura_lang_en_US.dll'; sha256 = 'sha256:' + (Get-Sha256 (Join-Path $closureSourceRoot 'sakura_lang_en_US.dll')); size = [UInt64]$closureResourceInfo.Length }
        )
        $closureReceipt = [ordered]@{ schema_version = 1; context_id = 'msvc-x64-debug'; staging_set_id = 'selftest-runtime-stage'; files = $closureEntries }
        $closureReceiptPath = Join-Path $closureSourceRoot '.sakura-runtime-stage.json'
        [IO.File]::WriteAllText($closureReceiptPath, ($closureReceipt | ConvertTo-Json -Depth 5), (New-Object Text.UTF8Encoding($false)))
        [IO.Directory]::CreateDirectory($closureBundleRoot) | Out-Null
        $closureBundle = New-StartupArtifactBundle $closureSourceRoot $closureBundleRoot 'startup-probe-bundle-closure-selftest' $closureReceiptPath
        $closureBundleVerification = Assert-StartupArtifactBundleUnchanged $closureBundle
        if ($closureBundle.closure.mode -ne 'runtime-stage-receipt' -or $closureBundle.closure.files.Count -ne 2 -or
            $closureBundle.closure.copiedFiles.Count -ne 2 -or -not $closureBundleVerification.sourceClosureUnchanged -or
            -not $closureBundleVerification.copiedClosureUnchanged -or
            -not (Test-Path -LiteralPath (Join-Path $closureBundle.bundlePath 'sakura_lang_en_US.dll'))) {
            throw 'Runtime artifact closure self-test failed.'
        }
        if ($closureBundle.closure.files[0].canonicalRelativePath -notmatch '^build\\staging\\msvc-x64-debug\\sakura-editor\\') {
            throw 'Runtime receipt canonical path was not retained in the closure.'
        }
        $closureReceiptOriginal = [IO.File]::ReadAllText($closureReceiptPath)
        $badReceiptCases = @(
            [pscustomobject]@{ destination = 'C:\staging\sakura.exe'; source = 'x64/Debug/sakura.exe'; artifactId = 'sakura-editor-msvc-x64-debug-product' }
            [pscustomobject]@{ destination = 'build/staging/msvc-x64-debug/sakura-editor/../sakura.exe'; source = 'x64/Debug/sakura.exe'; artifactId = 'sakura-editor-msvc-x64-debug-product' }
            [pscustomobject]@{ destination = 'build/staging/wrong/sakura-editor/sakura.exe'; source = 'x64/Debug/sakura.exe'; artifactId = 'sakura-editor-msvc-x64-debug-product' }
            [pscustomobject]@{ destination = 'build/staging/msvc-x64-debug/sakura-editor/sakura.exe'; source = 'x64/Release/sakura.exe'; artifactId = 'sakura-editor-msvc-x64-debug-product' }
            [pscustomobject]@{ destination = 'build/staging/msvc-x64-debug/sakura-editor/sakura.exe'; source = 'x64/Debug/sakura.exe:ads'; artifactId = 'sakura-editor-msvc-x64-debug-product' }
            [pscustomobject]@{ destination = 'build/staging/msvc-x64-debug/sakura-editor/sakura.exe'; source = 'x64/Debug/sakura.exe'; artifactId = 'wrong-editor-id' }
        )
        foreach ($badCase in $badReceiptCases) {
            $badReceipt = $closureReceiptOriginal | ConvertFrom-Json
            $badReceipt.files[0].destination = $badCase.destination
            $badReceipt.files[0].source = $badCase.source
            $badReceipt.files[0].artifact_id = $badCase.artifactId
            [IO.File]::WriteAllText($closureReceiptPath, ($badReceipt | ConvertTo-Json -Depth 5), (New-Object Text.UTF8Encoding($false)))
            $badRejected = $false
            try { [void](Get-StartupArtifactClosureManifest $closureSourceRoot $closureReceiptPath) } catch { $badRejected = $true }
            if (-not $badRejected) { throw 'Unsafe runtime receipt path self-test was accepted.' }
        }
        foreach ($unsafePath in @('foo \bar', 'foo.', 'NUL.dll', 'COM1.txt', 'LPT9')) {
            $unsafePathRejected = $false
            try { [void](Convert-StartupReceiptPath $unsafePath 'self-test') } catch { $unsafePathRejected = $true }
            if (-not $unsafePathRejected) { throw 'Unsafe Windows runtime receipt path self-test was accepted.' }
        }
        $nestedLanguageRejected = $false
        try {
            Assert-StartupReceiptArtifactIdentity 'sakura-language-en-us-resource' 'language-en-us' 'nested\sakura_lang_en_US.dll' 'msvc-x64-debug'
        }
        catch { $nestedLanguageRejected = $true }
        if (-not $nestedLanguageRejected) { throw 'Nested known language runtime receipt identity self-test was accepted.' }
        [IO.File]::WriteAllText($closureReceiptPath, $closureReceiptOriginal, (New-Object Text.UTF8Encoding($false)))
        # ConvertTo-Json uses two spaces before values in Windows PowerShell 5.1
        # and one in PowerShell 7.  Mutate the parsed document so this tamper
        # check exercises the receipt schema on both hosts.
        $tamperedReceipt = $closureReceiptOriginal | ConvertFrom-Json
        $tamperedReceipt.schema_version = 2
        [IO.File]::WriteAllText($closureReceiptPath, ($tamperedReceipt | ConvertTo-Json -Depth 5), (New-Object Text.UTF8Encoding($false)))
        $receiptRejected = $false
        try { [void](Get-StartupArtifactClosureManifest $closureSourceRoot $closureReceiptPath) } catch { $receiptRejected = $true }
        if (-not $receiptRejected) { throw 'Tampered runtime artifact receipt was accepted.' }
        [IO.File]::WriteAllText($closureReceiptPath, $closureReceiptOriginal, (New-Object Text.UTF8Encoding($false)))
        $copiedResourcePath = Join-Path $closureBundle.bundlePath 'sakura_lang_en_US.dll'
        $copiedResourceOriginal = [IO.File]::ReadAllBytes($copiedResourcePath)
        $copiedResourceTampered = [byte[]]$copiedResourceOriginal.Clone()
        $copiedResourceTampered[0] = $copiedResourceTampered[0] -bxor 1
        [IO.File]::WriteAllBytes($copiedResourcePath, $copiedResourceTampered)
        $copiedHashRejected = $false
        try { [void](Assert-StartupArtifactBundleUnchanged $closureBundle) } catch { $copiedHashRejected = $true }
        [IO.File]::WriteAllBytes($copiedResourcePath, $copiedResourceOriginal)
        if (-not $copiedHashRejected) { throw 'Tampered copied runtime artifact was accepted.' }
        [void](Assert-StartupArtifactBundleUnchanged $closureBundle)
        $startInfoSelfTest = New-Object Diagnostics.ProcessStartInfo
        $startInfoSelfTest.WorkingDirectory = $closureBundle.bundlePath
        if (-not (Test-SamePath $startInfoSelfTest.WorkingDirectory $closureBundle.bundlePath)) { throw 'Process working-directory self-test failed.' }
    }
    finally {
        if ($null -ne $closureBundle) { try { Remove-StartupArtifactBundle $closureBundle } catch { } }
        if (Test-Path -LiteralPath $closureBundleRoot) { [IO.Directory]::Delete($closureBundleRoot, $true) }
        if (Test-Path -LiteralPath $closureSourceRoot) { [IO.Directory]::Delete($closureSourceRoot, $true) }
    }
    $jobSelfTest = [NativeStartupProbe]::CreateKillOnCloseJob()
    if (-not $jobSelfTest.Succeeded -or $jobSelfTest.Handle -eq [IntPtr]::Zero) { throw 'Kill-on-close job creation self-test failed.' }
    $jobSelfTestClosed = $false
    try {
        $emptyJob = [NativeStartupProbe]::QueryJobProcessIds($jobSelfTest.Handle)
        if (-not $emptyJob.Succeeded -or $null -eq $emptyJob.ProcessIds -or $emptyJob.ProcessIds.Count -ne 0) { throw 'Empty kill-on-close job query self-test failed.' }
    }
    finally {
        $jobSelfTestCloseResult = [NativeStartupProbe]::CloseKillOnCloseJob($jobSelfTest.Handle)
        $jobSelfTestClosed = [bool]$jobSelfTestCloseResult.Succeeded
    }
    if (-not $jobSelfTestClosed) { throw 'Kill-on-close job cleanup self-test failed.' }
    $emptyJobObservation = Convert-StartupJobQueryObservation $emptyJob
    $emptyJobObservationSelfTestVerified = $emptyJobObservation.attempted -and $emptyJobObservation.queryCount -eq 1 -and
        $emptyJobObservation.attemptCount -ge 1 -and
        $emptyJobObservation.attempts.Count -le 8 -and $emptyJobObservation.attempts.Count -ge 1 -and
        $emptyJobObservation.attempts[0].capacityBytes -eq 0
    if (-not $emptyJobObservationSelfTestVerified) {
        throw 'Bounded empty-job query observation self-test failed.'
    }
    $jobQueryObservationSelfTestVerified = $emptyJobObservationSelfTestVerified
    foreach ($observationField in @('attempted', 'attemptCount', 'capacityBytes', 'requiredBytes',
            'returnLengthBytes', 'assignedProcessCount', 'listedProcessCount', 'resized', 'attempts')) {
        if (-not $emptyJobObservation.Contains($observationField)) {
            $jobQueryObservationSelfTestVerified = $false
            break
        }
    }
    if (-not $jobQueryObservationSelfTestVerified) { throw 'Job query observation field self-test failed.' }
    # Enter the owning try/finally immediately after creation.  Even if either
    # verification list allocation fails, the job handle remains covered by the
    # kill-on-close cleanup fence.
    $multiMemberJob = [NativeStartupProbe]::CreateKillOnCloseJob()
    $multiMemberChildren = $null
    $multiMemberExpectedIds = $null
    $multiMemberJobQuery = $null
    $realMultiMemberJobQuerySelfTestVerified = $false
    $realMultiMemberJobTerminationSelfTestVerified = $false
    $multiMemberCleanupVerified = $false
    try {
        if (-not $multiMemberJob.Succeeded -or $multiMemberJob.Handle -eq [IntPtr]::Zero) {
            throw 'Multi-member kill-on-close job creation self-test failed.'
        }
        $multiMemberChildren = New-Object Collections.Generic.List[object]
        $multiMemberExpectedIds = New-Object Collections.Generic.List[int]
        $multiMemberCommandLine = '"{0}" /c pause' -f $env:ComSpec
        for ($memberIndex = 0; $memberIndex -lt 2; $memberIndex++) {
            $child = [NativeStartupProbe]::CreateSuspendedProcess(
                $env:ComSpec, $multiMemberCommandLine, $env:SystemRoot, [string[]]@())
            if (-not $child.Succeeded -or $child.ProcessHandle -eq [IntPtr]::Zero -or
                $child.ThreadHandle -eq [IntPtr]::Zero -or $child.ProcessId -le 0) {
                throw 'Multi-member suspended process creation self-test failed.'
            }
            try {
                [void]$multiMemberChildren.Add($child)
                [void]$multiMemberExpectedIds.Add([int]$child.ProcessId)
            }
            catch {
                # If list growth itself fails, close this untracked child
                # before the outer job fence is retried.
                try { [void][NativeStartupProbe]::TerminateProcessHandle($child.ProcessHandle) } catch { }
                try { [void][NativeStartupProbe]::CloseNativeHandle($child.ThreadHandle) } catch { }
                try { [void][NativeStartupProbe]::CloseNativeHandle($child.ProcessHandle) } catch { }
                throw
            }
            $childAssigned = [NativeStartupProbe]::AssignProcessToKillOnCloseJob(
                $multiMemberJob.Handle, $child.ProcessId)
            if (-not $childAssigned.Succeeded) {
                throw 'Multi-member job assignment self-test failed.'
            }
        }
        $multiMemberJobQuery = [NativeStartupProbe]::QueryJobProcessIds($multiMemberJob.Handle)
        $multiMemberIds = @($multiMemberJobQuery.ProcessIds | ForEach-Object { [int]$_ })
        $multiMemberActualSorted = @($multiMemberIds | Sort-Object)
        $multiMemberExpectedSorted = @($multiMemberExpectedIds | Sort-Object)
        $multiMemberIdsMatch = ($multiMemberActualSorted -join ',') -eq ($multiMemberExpectedSorted -join ',')
        $multiMemberAttemptsInBounds = $true
        foreach ($multiMemberAttempt in @($multiMemberJobQuery.Attempts)) {
            if ($null -eq $multiMemberAttempt -or
                [UInt64]$multiMemberAttempt.CapacityBytes -gt 1048576 -or
                [UInt64]$multiMemberAttempt.RequiredBytes -gt 1048576 -or
                [UInt64]$multiMemberAttempt.ReturnLengthBytes -gt 1048576) {
                $multiMemberAttemptsInBounds = $false
                break
            }
        }
        $multiMemberFinalAttempt = @($multiMemberJobQuery.Attempts)[-1]
        $multiMemberFinalComplete = $null -ne $multiMemberFinalAttempt -and
            $multiMemberFinalAttempt.Succeeded -and
            $multiMemberFinalAttempt.AssignedProcessCount -eq 2 -and
            $multiMemberFinalAttempt.ListedProcessCount -eq 2
        $realMultiMemberJobQuerySelfTestVerified = $multiMemberJobQuery.Succeeded -and
            $multiMemberJobQuery.AttemptCount -ge 1 -and $multiMemberJobQuery.AttemptCount -le 8 -and
            $multiMemberJobQuery.AssignedProcessCount -eq 2 -and
            $multiMemberJobQuery.ListedProcessCount -eq 2 -and
            [UInt64]$multiMemberJobQuery.CapacityBytes -le 1048576 -and
            [UInt64]$multiMemberJobQuery.RequiredBytes -le 1048576 -and
            [UInt64]$multiMemberJobQuery.ReturnLengthBytes -le 1048576 -and
            $multiMemberIds.Count -eq 2 -and
            $multiMemberIds[0] -gt 0 -and $multiMemberIds[1] -gt 0 -and
            $multiMemberIds[0] -ne $multiMemberIds[1] -and
            $multiMemberIdsMatch -and $multiMemberAttemptsInBounds -and
            $multiMemberFinalComplete
        if (-not $realMultiMemberJobQuerySelfTestVerified) {
            throw 'Multi-member job query self-test failed.'
        }
        $multiMemberTermination = [NativeStartupProbe]::TerminateKillOnCloseJob($multiMemberJob.Handle)
        if (-not $multiMemberTermination.Attempted -or -not $multiMemberTermination.Succeeded -or
            $multiMemberTermination.ErrorCode -ne 0) {
            throw 'Multi-member exact-Job termination self-test failed.'
        }
        $multiMemberTerminalQuery = $null
        $multiMemberTerminalPollCount = 0
        $multiMemberTerminalWatch = [Diagnostics.Stopwatch]::StartNew()
        while ($multiMemberTerminalPollCount -lt $startupContainmentMaxOuterPolls -and
                $multiMemberTerminalWatch.ElapsedMilliseconds -lt $closeTimeoutMs) {
            $multiMemberTerminalPollCount++
            $multiMemberTerminalQuery = [NativeStartupProbe]::QueryJobProcessIds($multiMemberJob.Handle)
            $multiMemberTerminal = Convert-StartupTerminalJobQuery $multiMemberTerminalQuery
            if (-not $multiMemberTerminal.valid) { break }
            if ($multiMemberTerminal.memberCount -eq 0) {
                $realMultiMemberJobTerminationSelfTestVerified = $true
                break
            }
            Start-Sleep -Milliseconds 10
        }
        if (-not $realMultiMemberJobTerminationSelfTestVerified -or
            $multiMemberTerminalPollCount -lt 1 -or
            $multiMemberTerminalPollCount -gt $startupContainmentMaxOuterPolls -or
            $multiMemberTerminalWatch.ElapsedMilliseconds -ge $closeTimeoutMs) {
            throw 'TerminateJobObject did not leave the Job handle queryable through a bounded empty-membership proof.'
        }
    }
    finally {
        # The exact Job termination/query proof above is primary evidence.  The
        # close remains the KILL_ON_JOB_CLOSE fail-safe, and exact process handles
        # independently prove this self-test left no suspended cmd.exe child.
        $multiMemberJobCloseSucceeded = $false
        if ($null -ne $multiMemberJob -and $multiMemberJob.Handle -ne [IntPtr]::Zero) {
            try {
                $multiMemberJobClose = [NativeStartupProbe]::CloseKillOnCloseJob($multiMemberJob.Handle)
                $multiMemberJobCloseSucceeded = [bool]$multiMemberJobClose.Succeeded
            }
            catch { $multiMemberJobCloseSucceeded = $false }
        }
        $multiMemberChildrenExited = $true
        for ($childIndex = 0; $null -ne $multiMemberChildren -and $childIndex -lt $multiMemberChildren.Count; $childIndex++) {
            $child = $multiMemberChildren[$childIndex]
            if ($null -eq $child -or $child.ProcessHandle -eq [IntPtr]::Zero) { continue }
            $exitObserved = $false
            $exitWatch = [Diagnostics.Stopwatch]::StartNew()
            while ($exitWatch.ElapsedMilliseconds -lt 3000) {
                try {
                    $exitProbe = [NativeStartupProbe]::QueryProcessExitState($child.ProcessHandle)
                    if ($exitProbe.Succeeded -and -not $exitProbe.Active) {
                        $exitObserved = $true
                        break
                    }
                }
                catch { }
                Start-Sleep -Milliseconds 10
            }
            if (-not $exitObserved) {
                try { [void][NativeStartupProbe]::TerminateProcessHandle($child.ProcessHandle) } catch { }
                $exitWatch.Restart()
                while ($exitWatch.ElapsedMilliseconds -lt 1000) {
                    try {
                        $exitProbe = [NativeStartupProbe]::QueryProcessExitState($child.ProcessHandle)
                        if ($exitProbe.Succeeded -and -not $exitProbe.Active) {
                            $exitObserved = $true
                            break
                        }
                    }
                    catch { }
                    Start-Sleep -Milliseconds 10
                }
            }
            if (-not $exitObserved) { $multiMemberChildrenExited = $false }
            if ($child.ThreadHandle -ne [IntPtr]::Zero) {
                try { [void][NativeStartupProbe]::CloseNativeHandle($child.ThreadHandle) } catch { }
            }
            try { [void][NativeStartupProbe]::CloseNativeHandle($child.ProcessHandle) } catch { }
        }
        if (-not $multiMemberJobCloseSucceeded -and
            $null -ne $multiMemberJob -and $multiMemberJob.Handle -ne [IntPtr]::Zero) {
            try {
                # A transient first CloseHandle failure is retried and its
                # actual result is part of the cleanup proof.
                $multiMemberJobCloseRetry = [NativeStartupProbe]::CloseKillOnCloseJob($multiMemberJob.Handle)
                $multiMemberJobCloseSucceeded = [bool]$multiMemberJobCloseRetry.Succeeded
            }
            catch { $multiMemberJobCloseSucceeded = $false }
        }
        $multiMemberCleanupVerified = $multiMemberJobCloseSucceeded -and $multiMemberChildrenExited -and
            $realMultiMemberJobTerminationSelfTestVerified
    }
    if (-not $multiMemberCleanupVerified) {
        throw 'Multi-member job cleanup self-test could not prove child exit.'
    }

    # Deterministic, payload-free state-machine cases.  These invoke no GUI and
    # use a nonzero sentinel only through injected invokers; no native API sees it.
    $newTerminalJobQuery = {
        param([int[]]$ProcessIds)
        $ids = [int[]]@($ProcessIds)
        $listed = [UInt32]$ids.Length
        $capacity = [UInt64]($startupJobQueryHeaderBytes +
            ([UInt64][Math]::Max(1, $ids.Length) * [UInt64]([IntPtr]::Size)))
        $returned = [UInt64]($startupJobQueryHeaderBytes + ([UInt64]$ids.Length * [UInt64]([IntPtr]::Size)))
        $attempt = [pscustomobject][ordered]@{
            Attempted = $true; AttemptNumber = 1; Succeeded = $true; ErrorCode = 0
            CapacityBytes = $capacity; RequiredBytes = $capacity; ReturnLengthBytes = $returned
            AssignedProcessCount = $listed; ListedProcessCount = $listed; Resized = $false
        }
        return [pscustomobject][ordered]@{
            Attempted = $true; Succeeded = $true; ErrorCode = 0; ProcessIds = $ids
            AttemptCount = 1; CapacityBytes = $capacity; RequiredBytes = $capacity; ReturnLengthBytes = $returned
            AssignedProcessCount = $listed; ListedProcessCount = $listed; Resized = $false
            Attempts = [object[]]@($attempt)
        }
    }
    $newCloseSuccess = { [pscustomobject][ordered]@{ Succeeded = $true; ErrorCode = 0; Handle = [IntPtr]::Zero } }
    $newTerminationSuccess = { [pscustomobject][ordered]@{ Attempted = $true; Succeeded = $true; ErrorCode = 0 } }
    $syntheticMemberQueryFixture = & $newTerminalJobQuery ([int[]]@(1234))
    $syntheticMemberQueryCheck = Convert-StartupTerminalJobQuery $syntheticMemberQueryFixture
    if (-not $syntheticMemberQueryCheck.valid -or $syntheticMemberQueryCheck.memberCount -ne 1) {
        throw "Synthetic terminal Job query fixture failed '$($syntheticMemberQueryCheck.reason)'."
    }
    $requiredMismatchQuery = & $newTerminalJobQuery ([int[]]@())
    $requiredMismatchQuery.RequiredBytes = [UInt64]($requiredMismatchQuery.RequiredBytes + [UInt64]8)
    $requiredMismatchStrictCheck = Test-StartupStrictJobQueryEnvelope $requiredMismatchQuery
    $requiredMismatchTerminalCheck = Convert-StartupTerminalJobQuery $requiredMismatchQuery
    $strictTerminalJobQuerySelfTestVerified = -not $requiredMismatchStrictCheck.valid -and
        $requiredMismatchStrictCheck.reason -eq 'top-level-final-equation' -and
        -not $requiredMismatchTerminalCheck.valid -and
        $requiredMismatchTerminalCheck.reason -eq 'top-level-final-equation'
    if (-not $strictTerminalJobQuerySelfTestVerified) {
        throw 'Terminal Job query accepted mismatched top-level and final-attempt RequiredBytes.'
    }

    $malformedGracefulState = [pscustomobject]@{ query = 0; terminate = 0; close = 0 }
    $malformedGracefulInvokers = [pscustomobject]@{
        ObserveGraceful = { 'true' }
        QueryJob = { $malformedGracefulState.query++; & $newTerminalJobQuery ([int[]]@()) }
        TerminateJob = { $malformedGracefulState.terminate++; & $newTerminationSuccess }
        CloseJob = { $malformedGracefulState.close++; & $newCloseSuccess }
        Delay = { param([int]$Milliseconds) }
    }
    $malformedGracefulResult = Invoke-StartupJobCleanupStateMachine ([IntPtr]1) @{} `
        (New-StartupCleanupObservation) $malformedGracefulInvokers 250
    $strictGracefulObservationSelfTestVerified = -not $malformedGracefulResult.graceful -and
        $malformedGracefulState.terminate -eq 1 -and $malformedGracefulState.query -eq 1 -and
        $malformedGracefulState.close -eq 1 -and
        $malformedGracefulResult.proof.terminalState -eq 'verified-explicit-job-termination' -and
        $malformedGracefulResult.proof.mode -eq 'explicit-job-termination' -and
        (Test-StartupContainmentProofV2 $malformedGracefulResult.proof)
    if (-not $strictGracefulObservationSelfTestVerified) {
        throw 'Malformed graceful observation influenced a graceful containment transition.'
    }

    $identityFailureState = [pscustomobject]@{ query = 0; terminate = 0; close = 0 }
    $identityFailureObservation = New-StartupCleanupObservation
    $identityFailureInvokers = [pscustomobject]@{
        ObserveGraceful = {
            Set-StartupJobIdentityFailure $identityFailureObservation.jobIdentityObservation `
                'identity-still-present' 5 'open-process' 'graceful-job-member'
            throw 'Synthetic open-process identity failure with member still listed.'
        }
        QueryJob = {
            $identityFailureState.query++
            if ($identityFailureState.query -eq 1) { & $newTerminalJobQuery ([int[]]@(1234)) }
            else { & $newTerminalJobQuery ([int[]]@()) }
        }
        TerminateJob = { $identityFailureState.terminate++; & $newTerminationSuccess }
        CloseJob = { $identityFailureState.close++; & $newCloseSuccess }
        Delay = { param([int]$Milliseconds) }
    }
    $identityFailureWatch = [Diagnostics.Stopwatch]::StartNew()
    $identityFailureResult = Invoke-StartupJobCleanupStateMachine ([IntPtr]1) @{} `
        $identityFailureObservation $identityFailureInvokers 250
    $identityFailureWatch.Stop()
    $openProcessIdentityEscalationSelfTestVerified =
        $identityFailureState.terminate -eq 1 -and $identityFailureState.close -eq 1 -and
        $identityFailureState.query -eq 2 -and $identityFailureResult.outerPollCount -eq 2 -and
        $identityFailureResult.proof.terminalState -eq 'verified-explicit-job-termination' -and
        $identityFailureResult.proof.jobEmptyProven -and
        $identityFailureObservation.jobIdentityObservation.failureOperation -eq 'open-process' -and
        $identityFailureObservation.jobIdentityObservation.observerRole -eq 'graceful-job-member' -and
        $identityFailureWatch.ElapsedMilliseconds -lt 250
    if (-not $openProcessIdentityEscalationSelfTestVerified) {
        throw 'Open-process identity failure did not trigger exactly one bounded exact-Job termination.'
    }

    $explicitProof = $identityFailureResult.proof
    $explicitObservation = New-StartupCleanupObservation
    $trackedReconciliationAccepted = Invoke-StartupTrackedIdentityFailure $explicitObservation 1234 `
        ([pscustomobject][ordered]@{ Succeeded = $false; ErrorCode = 5; Operation = 'open-process'; Identity = $null }) `
        { [pscustomobject][ordered]@{
                Attempted = $true; Complete = $true; Succeeded = $true; ErrorCode = 0
                AttemptCount = 1; RetryCount = 0; Retried = $false; Entries = [object[]]@()
            } } `
        'Synthetic post-close tracked-history identity failure.' $true 'post-close-tracked-history' $explicitProof
    $explicitObservation.containmentProof = $explicitProof
    $explicitObservation.jobQuerySucceeded = $true
    $explicitObservation.jobCloseSucceeded = $true
    $explicitObservation.trackedSweepVerified = $true
    $explicitObservation.finalPathSweepVerified = $true
    $explicitCleanup = [pscustomobject][ordered]@{
        survivors = @(); jobQuerySucceeded = $true; jobCloseSucceeded = $true
        finalPathSweepVerified = $true; containmentProof = $explicitProof
        cleanupObservation = $explicitObservation
    }
    $verifiedExplicitTerminationSelfTestVerified = $trackedReconciliationAccepted -and
        (Test-StartupProcessCleanupCompletion $explicitCleanup ([IntPtr]::Zero) 0) -and
        $explicitProof.identityReconciliation.reason -eq 'job-empty-and-allowlisted-post-close-history'
    if (-not $verifiedExplicitTerminationSelfTestVerified) {
        throw 'Verified explicit-Job-termination proof with allowlisted tracked-history reconciliation was rejected.'
    }

    $memberRemainsState = [pscustomobject]@{ query = 0; terminate = 0; close = 0 }
    $memberRemainsInvokers = [pscustomobject]@{
        ObserveGraceful = { $false }
        QueryJob = { $memberRemainsState.query++; & $newTerminalJobQuery ([int[]]@(1234)) }
        TerminateJob = { $memberRemainsState.terminate++; & $newTerminationSuccess }
        CloseJob = { $memberRemainsState.close++; & $newCloseSuccess }
        Delay = { param([int]$Milliseconds) }
    }
    $memberRemainsResult = Invoke-StartupJobCleanupStateMachine ([IntPtr]1) @{} `
        (New-StartupCleanupObservation) $memberRemainsInvokers 250

    $malformedState = [pscustomobject]@{ query = 0; terminate = 0; close = 0 }
    $malformedInvokers = [pscustomobject]@{
        ObserveGraceful = { $false }
        QueryJob = { $malformedState.query++; [pscustomobject]@{ Succeeded = $true } }
        TerminateJob = { $malformedState.terminate++; & $newTerminationSuccess }
        CloseJob = { $malformedState.close++; & $newCloseSuccess }
        Delay = { param([int]$Milliseconds) }
    }
    $malformedResult = Invoke-StartupJobCleanupStateMachine ([IntPtr]1) @{} `
        (New-StartupCleanupObservation) $malformedInvokers 250
    $malformedReason = (Convert-StartupTerminalJobQuery ([pscustomobject]@{ Succeeded = $true })).reason

    $failedQueryState = [pscustomobject]@{ query = 0; terminate = 0; close = 0 }
    $failedQueryInvokers = [pscustomobject]@{
        ObserveGraceful = { $false }
        QueryJob = {
            $failedQueryState.query++
            [pscustomobject][ordered]@{
                Attempted = $true; Succeeded = $false; ErrorCode = 5; ProcessIds = [int[]]@()
                AttemptCount = 1; CapacityBytes = [UInt64]0; RequiredBytes = [UInt64]0; ReturnLengthBytes = [UInt64]0
                AssignedProcessCount = [UInt32]0; ListedProcessCount = [UInt32]0; Resized = $false
                Attempts = [object[]]@([pscustomobject][ordered]@{
                    Attempted = $true; AttemptNumber = 1; Succeeded = $false; ErrorCode = 5
                    CapacityBytes = [UInt64]0; RequiredBytes = [UInt64]0; ReturnLengthBytes = [UInt64]0
                    AssignedProcessCount = [UInt32]0; ListedProcessCount = [UInt32]0; Resized = $false
                })
            }
        }
        TerminateJob = { $failedQueryState.terminate++; & $newTerminationSuccess }
        CloseJob = { $failedQueryState.close++; & $newCloseSuccess }
        Delay = { param([int]$Milliseconds) }
    }
    $failedQueryResult = Invoke-StartupJobCleanupStateMachine ([IntPtr]1) @{} `
        (New-StartupCleanupObservation) $failedQueryInvokers 250

    $terminationFailureState = [pscustomobject]@{ terminate = 0; close = 0 }
    $terminationFailureInvokers = [pscustomobject]@{
        ObserveGraceful = { $false }
        QueryJob = { & $newTerminalJobQuery ([int[]]@()) }
        TerminateJob = { $terminationFailureState.terminate++; [pscustomobject]@{ Attempted = $true; Succeeded = $false; ErrorCode = [int]5 } }
        CloseJob = { $terminationFailureState.close++; & $newCloseSuccess }
        Delay = { param([int]$Milliseconds) }
    }
    $terminationFailureResult = Invoke-StartupJobCleanupStateMachine ([IntPtr]1) @{} `
        (New-StartupCleanupObservation) $terminationFailureInvokers 250

    $closeFailureState = [pscustomobject]@{ terminate = 0; close = 0 }
    $closeFailureInvokers = [pscustomobject]@{
        ObserveGraceful = { $false }
        QueryJob = { & $newTerminalJobQuery ([int[]]@()) }
        TerminateJob = { $closeFailureState.terminate++; & $newTerminationSuccess }
        CloseJob = { $closeFailureState.close++; [pscustomobject]@{ Succeeded = $false; ErrorCode = 6; Handle = [IntPtr]1 } }
        Delay = { param([int]$Milliseconds) }
    }
    $closeFailureResult = Invoke-StartupJobCleanupStateMachine ([IntPtr]1) @{} `
        (New-StartupCleanupObservation) $closeFailureInvokers 250

    $exceptionState = [pscustomobject]@{ close = 0 }
    $exceptionInvokers = [pscustomobject]@{
        ObserveGraceful = 'malformed'
        CloseJob = { $exceptionState.close++; & $newCloseSuccess }
    }
    $exceptionResult = Invoke-StartupJobCleanupStateMachine ([IntPtr]1) @{} `
        (New-StartupCleanupObservation) $exceptionInvokers 250

    $malformedTerminationActions = @(
        [pscustomobject][ordered]@{ Attempted = $false; Succeeded = $true; ErrorCode = [int]5 }
        [pscustomobject][ordered]@{ Attempted = $true; Succeeded = 'true'; ErrorCode = [int]0 }
        [pscustomobject][ordered]@{ Attempted = $true; Succeeded = $true; ErrorCode = [UInt32]0 }
        [pscustomobject][ordered]@{ Attempted = $true; Succeeded = $true; ErrorCode = [int]5 }
        [pscustomobject][ordered]@{ Attempted = $true; Succeeded = $false; ErrorCode = [int]0 }
    )
    $malformedTerminationActionsRejected = $true
    foreach ($malformedTerminationAction in $malformedTerminationActions) {
        $terminationActionForCase = $malformedTerminationAction
        $malformedTerminationState = [pscustomobject]@{ terminate = 0; query = 0; close = 0 }
        $malformedTerminationInvokers = [pscustomobject]@{
            ObserveGraceful = { $false }
            QueryJob = { $malformedTerminationState.query++; & $newTerminalJobQuery ([int[]]@()) }.GetNewClosure()
            TerminateJob = { $malformedTerminationState.terminate++; return $terminationActionForCase }.GetNewClosure()
            CloseJob = { $malformedTerminationState.close++; & $newCloseSuccess }.GetNewClosure()
            Delay = { param([int]$Milliseconds) }
        }
        $malformedTerminationResult = Invoke-StartupJobCleanupStateMachine ([IntPtr]1) @{} `
            (New-StartupCleanupObservation) $malformedTerminationInvokers 250
        if ($malformedTerminationResult.proof.terminalState -ne 'rejected-termination-failed' -or
            $malformedTerminationResult.proof.terminationSucceeded -or
            $malformedTerminationResult.proof.terminationErrorCode -le 0 -or
            $malformedTerminationState.terminate -ne 1 -or $malformedTerminationState.query -ne 0 -or
            $malformedTerminationState.close -ne 1 -or -not $malformedTerminationResult.closeSucceeded -or
            $malformedTerminationResult.remainingJobHandle -ne [IntPtr]::Zero -or
            -not (Test-StartupContainmentProofV2 $malformedTerminationResult.proof)) {
            $malformedTerminationActionsRejected = $false
        }
    }

    $malformedCloseActions = @(
        [pscustomobject][ordered]@{ Succeeded = $true; ErrorCode = [int]5; Handle = [IntPtr]1 }
        [pscustomobject][ordered]@{ Succeeded = $true; ErrorCode = [int]0; Handle = [IntPtr]1 }
        [pscustomobject][ordered]@{ Succeeded = 'true'; ErrorCode = [int]0; Handle = [IntPtr]::Zero }
        [pscustomobject][ordered]@{ Succeeded = $true; ErrorCode = [UInt32]0; Handle = [IntPtr]::Zero }
        [pscustomobject][ordered]@{ Succeeded = $true; ErrorCode = [int]0; Handle = [int]0 }
        [pscustomobject][ordered]@{ Succeeded = $false; ErrorCode = [int]0; Handle = [IntPtr]1 }
    )
    $malformedCloseActionsRejected = $true
    foreach ($malformedCloseAction in $malformedCloseActions) {
        $closeActionForCase = $malformedCloseAction
        $malformedCloseState = [pscustomobject]@{ terminate = 0; query = 0; close = 0 }
        $malformedCloseInvokers = [pscustomobject]@{
            ObserveGraceful = { $false }
            QueryJob = { $malformedCloseState.query++; & $newTerminalJobQuery ([int[]]@()) }.GetNewClosure()
            TerminateJob = { $malformedCloseState.terminate++; & $newTerminationSuccess }.GetNewClosure()
            CloseJob = { $malformedCloseState.close++; return $closeActionForCase }.GetNewClosure()
            Delay = { param([int]$Milliseconds) }
        }
        $malformedCloseResult = Invoke-StartupJobCleanupStateMachine ([IntPtr]1) @{} `
            (New-StartupCleanupObservation) $malformedCloseInvokers 250
        if ($malformedCloseResult.proof.terminalState -ne 'rejected-job-close' -or
            $malformedCloseResult.closeSucceeded -or $malformedCloseResult.remainingJobHandle -ne [IntPtr]1 -or
            -not $malformedCloseResult.proof.jobEmptyProven -or
            $malformedCloseState.terminate -ne 1 -or $malformedCloseState.query -ne 1 -or
            $malformedCloseState.close -ne 1 -or -not (Test-StartupContainmentProofV2 $malformedCloseResult.proof)) {
            $malformedCloseActionsRejected = $false
        }
    }
    $exactEnvelopeReproState = [pscustomobject]@{ terminate = 0; query = 0; close = 0 }
    $exactEnvelopeReproInvokers = [pscustomobject]@{
        ObserveGraceful = { $false }
        QueryJob = { $exactEnvelopeReproState.query++; & $newTerminalJobQuery ([int[]]@()) }
        TerminateJob = {
            $exactEnvelopeReproState.terminate++
            [pscustomobject][ordered]@{ Attempted = $false; Succeeded = $true; ErrorCode = [int]5 }
        }
        CloseJob = {
            $exactEnvelopeReproState.close++
            [pscustomobject][ordered]@{ Succeeded = $true; ErrorCode = [int]5; Handle = [IntPtr]1 }
        }
        Delay = { param([int]$Milliseconds) }
    }
    $exactEnvelopeReproResult = Invoke-StartupJobCleanupStateMachine ([IntPtr]1) @{} `
        (New-StartupCleanupObservation) $exactEnvelopeReproInvokers 250
    $strictNativeEnvelopeSelfTestVerified = $malformedTerminationActionsRejected -and
        $malformedCloseActionsRejected -and $exactEnvelopeReproState.terminate -eq 1 -and
        $exactEnvelopeReproState.query -eq 0 -and $exactEnvelopeReproState.close -eq 1 -and
        $exactEnvelopeReproResult.proof.terminalState -eq 'rejected-job-close' -and
        -not $exactEnvelopeReproResult.proof.jobEmptyProven -and
        -not $exactEnvelopeReproResult.closeSucceeded -and
        $exactEnvelopeReproResult.remainingJobHandle -eq [IntPtr]1 -and
        (Test-StartupContainmentProofV2 $exactEnvelopeReproResult.proof)
    if (-not $strictNativeEnvelopeSelfTestVerified) {
        throw 'Malformed termination or close action envelope crossed the containment decision boundary.'
    }

    $containmentFailureBranchesSelfTestVerified =
        (Test-StartupContainmentProofV2 $memberRemainsResult.proof) -and
        (Test-StartupContainmentProofV2 $malformedResult.proof) -and
        (Test-StartupContainmentProofV2 $failedQueryResult.proof) -and
        (Test-StartupContainmentProofV2 $terminationFailureResult.proof) -and
        (Test-StartupContainmentProofV2 $closeFailureResult.proof) -and
        (Test-StartupContainmentProofV2 $exceptionResult.proof) -and
        $memberRemainsResult.proof.terminalState -eq 'rejected-job-members-remain' -and
        $memberRemainsState.query -eq $startupContainmentMaxOuterPolls -and
        $memberRemainsState.terminate -eq 1 -and $memberRemainsState.close -eq 1 -and
        $malformedResult.proof.terminalState -eq 'rejected-terminal-job-query' -and
        $malformedReason -eq 'top-level-shape' -and
        $malformedState.query -eq 1 -and $malformedState.terminate -eq 1 -and $malformedState.close -eq 1 -and
        $failedQueryResult.proof.terminalState -eq 'rejected-terminal-job-query' -and
        $failedQueryState.query -eq 1 -and $failedQueryState.terminate -eq 1 -and $failedQueryState.close -eq 1 -and
        $terminationFailureResult.proof.terminalState -eq 'rejected-termination-failed' -and
        $terminationFailureState.terminate -eq 1 -and $terminationFailureState.close -eq 1 -and
        $closeFailureResult.proof.terminalState -eq 'rejected-job-close' -and
        $closeFailureState.terminate -eq 1 -and $closeFailureState.close -eq 1 -and
        $exceptionResult.proof.terminalState -eq 'exception' -and $exceptionState.close -eq 1 -and
        $memberRemainsResult.outerPollCount -le $startupContainmentMaxOuterPolls
    if (-not $containmentFailureBranchesSelfTestVerified) {
        throw 'One or more containment failure branches escaped its typed state, close finalizer, or query bound.'
    }

    $exactPathProof = New-StartupContainmentProof
    $exactPathProof.jobEmptyProven = $true
    $exactPathRejected = -not (Set-StartupIdentityReconciliation $exactPathProof 'open-process' 'exact-path') -and
        $exactPathProof.identityReconciliation.reason -eq 'exact-path-failure'
    $newVerifiedProofForRejectedReconciliation = {
        $candidateProof = New-StartupContainmentProof
        $candidateProof.mode = 'explicit-job-termination'
        $candidateProof.terminalState = 'verified-explicit-job-termination'
        $candidateProof.terminationAttempted = $true
        $candidateProof.terminationSucceeded = $true
        $candidateProof.terminalJobQuery = $explicitProof.terminalJobQuery
        $candidateProof.terminalJobMemberCount = 0
        $candidateProof.jobEmptyProven = $true
        return $candidateProof
    }
    $unknownOperationProof = & $newVerifiedProofForRejectedReconciliation
    $unknownOperationRejected = -not (Set-StartupIdentityReconciliation $unknownOperationProof `
            'arbitrary-operation-secret' 'post-close-tracked-history') -and
        $unknownOperationProof.identityReconciliation.operation -eq 'exception' -and
        $unknownOperationProof.identityReconciliation.observerRole -eq 'post-close-tracked-history' -and
        $unknownOperationProof.identityReconciliation.reason -eq 'operation-not-allowlisted' -and
        (Test-StartupContainmentProofV2 $unknownOperationProof)
    $unknownObserverProof = & $newVerifiedProofForRejectedReconciliation
    $unknownObserverRejected = -not (Set-StartupIdentityReconciliation $unknownObserverProof `
            'open-process' 'arbitrary-observer-secret') -and
        $unknownObserverProof.identityReconciliation.operation -eq 'open-process' -and
        $unknownObserverProof.identityReconciliation.observerRole -eq 'none' -and
        $unknownObserverProof.identityReconciliation.reason -eq 'observer-not-allowlisted' -and
        (Test-StartupContainmentProofV2 $unknownObserverProof)
    $rejectedReconciliationJson = @($unknownOperationProof, $unknownObserverProof) | ConvertTo-Json -Depth 10 -Compress
    $boundedRejectedReconciliationSelfTestVerified = $unknownOperationRejected -and $unknownObserverRejected -and
        $rejectedReconciliationJson -notmatch 'arbitrary-(operation|observer)-secret'
    if (-not $boundedRejectedReconciliationSelfTestVerified) {
        throw 'Rejected identity reconciliation retained unbounded operation or observer telemetry.'
    }
    $containmentEnumAndPayloadSelfTestVerified = $exactPathRejected -and $unknownOperationRejected -and
        $unknownObserverRejected -and (Test-StartupContainmentProofV2 $explicitProof) -and
        ($startupIdentityOperations -join ',') -eq 'none,open-process,query-image-path,get-process-times,exception' -and
        ($startupIdentityObserverRoles -join ',') -eq 'none,launch-job-member,graceful-job-member,post-close-tracked-history,exact-path' -and
        (($explicitProof | ConvertTo-Json -Depth 10 -Compress) -notmatch '(?i)processids|\bpid\b|imagepath|commandline|payload|raw')
    if (-not $containmentEnumAndPayloadSelfTestVerified) {
        throw 'Containment proof enum, reconciliation, or payload-free shape self-test failed.'
    }

    $noJobMembers = Get-StartupDiagnosticJobMembers ([IntPtr]::Zero)
    $noJobCleanup = Stop-OwnedProcesses @{} ([IntPtr]::Zero) $null
    $cleanupObservationSelfTestVerified = (-not $noJobMembers.verified -and $noJobMembers.queryObservation.skipped -and
        $noJobMembers.queryObservation.queryCount -eq 0 -and
        $noJobCleanup.cleanupObservation.jobQuerySkipped -and -not $noJobCleanup.cleanupObservation.jobQueryAttempted -and
        -not $noJobCleanup.cleanupObservation.query.attempted -and $noJobCleanup.cleanupObservation.query.attempts.Count -eq 0 -and
        -not $noJobCleanup.cleanupObservation.jobCloseAttempted -and -not $noJobCleanup.cleanupObservation.jobCloseSucceeded -and
        $noJobCleanup.containmentProof.version -eq 2 -and $noJobCleanup.containmentProof.mode -eq 'unavailable' -and
        $noJobCleanup.containmentProof.terminalState -eq 'rejected-job-unavailable' -and
        -not $noJobCleanup.containmentProof.jobEmptyProven -and
        $noJobCleanup.cleanupObservation.processEnumerationAttempted -and
        $noJobCleanup.cleanupObservation.processEnumerationSucceeded -and
        $noJobCleanup.cleanupObservation.processEnumerationComplete -and
        $noJobCleanup.cleanupObservation.processEnumerationCallCount -ge 1 -and
        $noJobCleanup.cleanupObservation.processEnumerationCompletedCount -ge 1 -and
        $noJobCleanup.cleanupObservation.processEnumerationFailureCount -eq 0 -and
        $noJobCleanup.cleanupObservation.processEnumerationErrorCode -eq $null -and
        $noJobCleanup.cleanupObservation.trackedSweepPassCount -ge 1)
    if (-not $cleanupObservationSelfTestVerified) {
        throw 'No-job query skip observation self-test failed.'
    }

    # Cleanup process-census telemetry is an aggregate over calls, not a new
    # decision surface.  Verify success, first-failure retention, saturation,
    # and the identity-gap classification/throw contract without serializing
    # any synthetic process identity.
    $successfulEnumerationObservation = New-StartupCleanupObservation
    $successfulEnumeration = [pscustomobject][ordered]@{
        Attempted = $true; AttemptCount = 1; Complete = $true; Succeeded = $true; ErrorCode = 0; RetryCount = 1; Entries = @()
    }
    $syntheticEnumerationShapeVerified = $null -ne $successfulEnumeration.PSObject.Properties['AttemptCount']
    if (-not $syntheticEnumerationShapeVerified) { throw 'Synthetic process-enumeration result shape self-test failed.' }
    Add-StartupProcessEnumerationObservation $successfulEnumerationObservation $successfulEnumeration
    Add-StartupProcessEnumerationObservation $successfulEnumerationObservation $successfulEnumeration
    $successfulEnumerationAggregateVerified = $successfulEnumerationObservation.processEnumerationAttempted -and
        $successfulEnumerationObservation.processEnumerationSucceeded -and
        $successfulEnumerationObservation.processEnumerationComplete -and
        $successfulEnumerationObservation.processEnumerationCallCount -eq 2 -and
        $successfulEnumerationObservation.processEnumerationCompletedCount -eq 2 -and
        $successfulEnumerationObservation.processEnumerationFailureCount -eq 0 -and
        $successfulEnumerationObservation.processEnumerationRetryCount -eq 2
    if (-not $successfulEnumerationAggregateVerified) { throw 'Successful process-enumeration aggregate self-test failed.' }
    $firstFailureEnumerationObservation = New-StartupCleanupObservation
    $firstEnumerationFailure = [pscustomobject][ordered]@{
        Attempted = $true; AttemptCount = 1; Complete = $false; Succeeded = $false; ErrorCode = 24; RetryCount = 2; Entries = @()
    }
    $laterEnumerationFailure = [pscustomobject][ordered]@{
        Attempted = $true; AttemptCount = 1; Complete = $false; Succeeded = $false; ErrorCode = 5; RetryCount = [int]::MaxValue; Entries = @()
    }
    Add-StartupProcessEnumerationObservation $firstFailureEnumerationObservation $firstEnumerationFailure
    Add-StartupProcessEnumerationObservation $firstFailureEnumerationObservation $laterEnumerationFailure
    Add-StartupProcessEnumerationObservation $firstFailureEnumerationObservation $successfulEnumeration
    $firstFailureEnumerationRetainedVerified = -not $firstFailureEnumerationObservation.processEnumerationSucceeded -and
        -not $firstFailureEnumerationObservation.processEnumerationComplete -and
        $firstFailureEnumerationObservation.processEnumerationErrorCode -eq 24 -and
        $firstFailureEnumerationObservation.processEnumerationFailureCount -eq 2 -and
        $firstFailureEnumerationObservation.processEnumerationCompletedCount -eq 1 -and
        $firstFailureEnumerationObservation.processEnumerationRetryCount -eq $startupObservationMaxCount
    if (-not $firstFailureEnumerationRetainedVerified) { throw 'First process-enumeration failure retention self-test failed.' }
    $boundedEnumerationObservation = New-StartupCleanupObservation
    for ($enumerationIndex = 0; $enumerationIndex -lt ($startupObservationMaxCount + 64); $enumerationIndex++) {
        Add-StartupProcessEnumerationObservation $boundedEnumerationObservation $laterEnumerationFailure
    }
    $boundedEnumerationCountsVerified = $boundedEnumerationObservation.processEnumerationCallCount -eq $startupObservationMaxCount -and
        $boundedEnumerationObservation.processEnumerationFailureCount -eq $startupObservationMaxCount -and
        $boundedEnumerationObservation.processEnumerationRetryCount -eq $startupObservationMaxCount
    if (-not $boundedEnumerationCountsVerified) { throw 'Bounded process-enumeration telemetry self-test failed.' }

    # Get-VerifiedProcessEntries is the trust boundary for every Toolhelp
    # census.  Exercise malformed initial envelopes here so string booleans,
    # scalar Entries, duplicate/fractional PIDs, and contradictory success
    # metadata can never be coerced into a containment decision.
    $malformedInitialCensusTooManyEntries = [Array]::CreateInstance([object], ($startupProcessEnumerationMaxEntryCount + 1))
    $malformedInitialCensusCases = @(
        [pscustomobject][ordered]@{ Attempted = 'false'; Complete = $true; Succeeded = $true; ErrorCode = 0; AttemptCount = 1; RetryCount = 0; Entries = @() }
        [pscustomobject][ordered]@{ Attempted = $true; Complete = 'true'; Succeeded = $true; ErrorCode = 0; AttemptCount = 1; RetryCount = 0; Entries = @() }
        [pscustomobject][ordered]@{ Attempted = $true; Complete = $true; Succeeded = 'true'; ErrorCode = 0; AttemptCount = 1; RetryCount = 0; Entries = @() }
        [pscustomobject][ordered]@{ Attempted = $true; Complete = $true; Succeeded = $true; ErrorCode = 5; AttemptCount = 1; RetryCount = 0; Entries = @() }
        [pscustomobject][ordered]@{ Attempted = $true; Complete = $true; Succeeded = $true; ErrorCode = [double]0; AttemptCount = 1; RetryCount = 0; Entries = @() }
        [pscustomobject][ordered]@{ Attempted = $true; Complete = $true; Succeeded = $true; ErrorCode = 0; AttemptCount = 1; RetryCount = 0; Entries = [pscustomobject]@{ ProcessId = 2222; ParentProcessId = 0; ImageName = 'self-test.exe' } }
        [pscustomobject][ordered]@{ Attempted = $true; Complete = $true; Succeeded = $true; ErrorCode = 0; AttemptCount = 1; RetryCount = 0; Entries = [object[]]@($null) }
        [pscustomobject][ordered]@{ Attempted = $true; Complete = $true; Succeeded = $true; ErrorCode = 0; AttemptCount = 1; RetryCount = 0; Entries = [object[]]@(
            [pscustomobject]@{ ProcessId = 2222; ParentProcessId = 0; ImageName = 'self-test.exe' }
            [pscustomobject]@{ ProcessId = 2222; ParentProcessId = 0; ImageName = 'self-test.exe' }
        ) }
        [pscustomobject][ordered]@{ Attempted = $true; Complete = $true; Succeeded = $true; ErrorCode = 0; AttemptCount = 1; RetryCount = 0; Entries = [object[]]@(
            [pscustomobject]@{ ProcessId = 2222.5; ParentProcessId = 0; ImageName = 'self-test.exe' }
        ) }
        [pscustomobject][ordered]@{ Attempted = $true; Complete = $true; Succeeded = $true; ErrorCode = 0; AttemptCount = 1; RetryCount = 0; Entries = [object[]]@(
            [pscustomobject]@{ ProcessId = '2222'; ParentProcessId = 0; ImageName = 'self-test.exe' }
        ) }
        [pscustomobject][ordered]@{ Attempted = $true; Complete = $true; Succeeded = $true; ErrorCode = 0; AttemptCount = 1; RetryCount = 0; Entries = [object[]]@(
            [pscustomobject]@{ ProcessId = 2222; ParentProcessId = -1; ImageName = 'self-test.exe' }
        ) }
        [pscustomobject][ordered]@{ Attempted = $true; Complete = $true; Succeeded = $true; ErrorCode = 0; AttemptCount = 1; RetryCount = 0; Entries = $malformedInitialCensusTooManyEntries }
        [pscustomobject][ordered]@{ Attempted = $true; Complete = $true; Succeeded = $true; ErrorCode = 0; AttemptCount = [double]1; RetryCount = 0; Retried = $false; Entries = @() }
        [pscustomobject][ordered]@{ Attempted = $true; Complete = $true; Succeeded = $true; ErrorCode = 0; AttemptCount = 1; RetryCount = [double]0; Retried = $false; Entries = @() }
        [pscustomobject][ordered]@{ Attempted = $true; Complete = $true; Succeeded = $true; ErrorCode = 0; AttemptCount = 1; RetryCount = 0; Retried = 'false'; Entries = @() }
        [pscustomobject][ordered]@{ Attempted = $true; Complete = $true; Succeeded = $true; ErrorCode = 0; AttemptCount = 1; RetryCount = 1; Retried = $true; Entries = @() }
        [pscustomobject][ordered]@{ Attempted = $true; Complete = $true; Succeeded = $true; ErrorCode = 0; AttemptCount = 4; RetryCount = 0; Retried = $false; Entries = @() }
    )
    $malformedInitialCensusRejected = $true
    foreach ($malformedInitialCensus in $malformedInitialCensusCases) {
        try {
            [void](Get-VerifiedProcessEntries -Invoker { return $malformedInitialCensus })
            $malformedInitialCensusRejected = $false
        }
        catch { }
    }
    $processEnumerationMalformedEnvelopeSelfTestVerified = [bool]$malformedInitialCensusRejected
    if (-not $processEnumerationMalformedEnvelopeSelfTestVerified) {
        throw 'Malformed initial process-enumeration envelope self-test failed.'
    }
    $processEnumerationStrictMetadataSelfTestVerified = [bool]$malformedInitialCensusRejected
    $pidZeroProcessEnumeration = [pscustomobject][ordered]@{
        Attempted = $true; Complete = $true; Succeeded = $true; ErrorCode = 0; AttemptCount = 1; RetryCount = 0; Retried = $false
        Entries = [object[]]@(
            [pscustomobject]@{ ProcessId = 0; ParentProcessId = 0; ImageName = 'System Idle Process' }
            [pscustomobject]@{ ProcessId = 2222; ParentProcessId = 0; ImageName = 'self-test.exe' }
        )
    }
    $pidZeroProcessEnumerationEntries = @(Get-VerifiedProcessEntries -Invoker { return $pidZeroProcessEnumeration })
    $processEnumerationPidZeroSelfTestVerified = [bool]($pidZeroProcessEnumerationEntries.Count -eq 2 -and
        [int]$pidZeroProcessEnumerationEntries[0].ProcessId -eq 0 -and
        [int]$pidZeroProcessEnumerationEntries[1].ProcessId -eq 2222 -and
        [string]$pidZeroProcessEnumerationEntries[0].ImageName -eq 'System Idle Process')
    if (-not $processEnumerationPidZeroSelfTestVerified) {
        throw 'PID 0 valid process-enumeration entry self-test failed.'
    }
    $failedProcessEnumerationObservation = New-StartupCleanupObservation
    $failedProcessEnumeration = [pscustomobject][ordered]@{
        Attempted = $true; AttemptCount = 1; Complete = $false; Succeeded = $false
        ErrorCode = 24; RetryCount = 0; Retried = $false; Entries = [object[]]@()
    }
    $failedProcessEnumerationThrew = $false
    try {
        [void](Get-VerifiedProcessEntries -Observation $failedProcessEnumerationObservation -Invoker { return $failedProcessEnumeration })
    }
    catch { $failedProcessEnumerationThrew = $true }
    $processEnumerationFailedEnvelopeSelfTestVerified = $failedProcessEnumerationThrew -and
        $failedProcessEnumerationObservation.processEnumerationCallCount -eq 1 -and
        $failedProcessEnumerationObservation.processEnumerationCompletedCount -eq 0 -and
        $failedProcessEnumerationObservation.processEnumerationFailureCount -eq 1 -and
        $failedProcessEnumerationObservation.processEnumerationErrorCode -eq 24
    if (-not $processEnumerationFailedEnvelopeSelfTestVerified) {
        throw 'Failed process-enumeration envelope self-test failed.'
    }

    $trackedTerminalObservation = New-StartupCleanupObservation
    Set-StartupTrackedSweepFailure $trackedTerminalObservation 'identity-disappeared' 5
    Set-StartupTrackedSweepFailure $trackedTerminalObservation 'identity-still-present' 6
    $trackedFirstFailureRetainedVerified = $trackedTerminalObservation.trackedSweepFailureType -eq 'identity-disappeared' -and
        $trackedTerminalObservation.trackedSweepFailureErrorCode -eq 5
    if (-not $trackedFirstFailureRetainedVerified) { throw 'First tracked-sweep failure retention self-test failed.' }
    $trackedFailureEnumsVerified = @(
        'none', 'identity-disappeared', 'identity-still-present', 'enumeration-unavailable',
        'identity-unavailable', 'exception'
    ) -join ',' -eq ($startupTrackedSweepFailureTypes -join ',')
    if (-not $trackedFailureEnumsVerified) { throw 'Tracked-sweep failure enum self-test failed.' }
    $boundedTrackedObservation = New-StartupCleanupObservation
    for ($trackedIndex = 0; $trackedIndex -lt ($startupObservationMaxCount + 64); $trackedIndex++) {
        Add-StartupTrackedSweepCount $boundedTrackedObservation 'trackedSweepIdentityAttemptCount'
        Add-StartupTrackedSweepCount $boundedTrackedObservation 'trackedSweepIdentityFailureCount'
        Add-StartupTrackedSweepCount $boundedTrackedObservation 'trackedSweepDisappearedAfterSnapshotCount'
        Add-StartupTrackedSweepCount $boundedTrackedObservation 'trackedSweepStillPresentAfterFailureCount'
        Add-StartupTrackedSweepCount $boundedTrackedObservation 'trackedSweepPassCount'
    }
    $boundedTrackedCountsVerified = $boundedTrackedObservation.trackedSweepIdentityAttemptCount -eq $startupObservationMaxCount -and
        $boundedTrackedObservation.trackedSweepIdentityFailureCount -eq $startupObservationMaxCount -and
        $boundedTrackedObservation.trackedSweepDisappearedAfterSnapshotCount -eq $startupObservationMaxCount -and
        $boundedTrackedObservation.trackedSweepStillPresentAfterFailureCount -eq $startupObservationMaxCount -and
        $boundedTrackedObservation.trackedSweepPassCount -eq $startupObservationMaxCount
    if (-not $boundedTrackedCountsVerified) { throw 'Bounded tracked-sweep telemetry self-test failed.' }

    $identityFailureProbe = [pscustomobject][ordered]@{ Succeeded = $false; ErrorCode = 5; Identity = $null }
    $identityDisappearedObservation = New-StartupCleanupObservation
    $freshAbsentCensusCalls = [pscustomobject]@{ Value = 0 }
    $freshAbsentCensus = [pscustomobject][ordered]@{
        Attempted = $true; AttemptCount = 1; Complete = $true; Succeeded = $true; ErrorCode = 0; RetryCount = 0; Retried = $false
        Entries = [object[]]@([pscustomobject]@{ ProcessId = 2222; ParentProcessId = 0; ImageName = 'self-test.exe' })
    }
    $identityDisappearedAccepted = $false
    try {
        $identityDisappearedAccepted = [bool](Invoke-StartupTrackedIdentityFailure $identityDisappearedObservation 1234 $identityFailureProbe {
            $freshAbsentCensusCalls.Value++
            return $freshAbsentCensus
        } 'Synthetic tracked identity failure.')
    }
    catch { $identityDisappearedAccepted = $false }
    $identityDisappearedVerified = $identityDisappearedAccepted -and $freshAbsentCensusCalls.Value -eq 1 -and
        $identityDisappearedObservation.trackedSweepFailureType -eq 'identity-disappeared' -and
        $identityDisappearedObservation.trackedSweepFailureErrorCode -eq 5 -and
        $identityDisappearedObservation.trackedSweepDisappearedAfterSnapshotCount -eq 1 -and
        $identityDisappearedObservation.trackedSweepStillPresentAfterFailureCount -eq 0
    if (-not $identityDisappearedVerified) { throw 'Tracked identity-disappeared self-test failed.' }

    $identityStillPresentObservation = New-StartupCleanupObservation
    $freshPresentCensusCalls = [pscustomobject]@{ Value = 0 }
    $freshPresentCensus = [pscustomobject][ordered]@{
        Attempted = $true; AttemptCount = 1; Complete = $true; Succeeded = $true; ErrorCode = 0; RetryCount = 0; Retried = $false
        Entries = [object[]]@([pscustomobject]@{ ProcessId = 1234; ParentProcessId = 0; ImageName = 'self-test.exe' })
    }
    $identityStillPresentThrew = $false
    try {
        Invoke-StartupTrackedIdentityFailure $identityStillPresentObservation 1234 $identityFailureProbe {
            $freshPresentCensusCalls.Value++
            return $freshPresentCensus
        } 'Synthetic tracked identity failure.'
    }
    catch { $identityStillPresentThrew = $true }
    $identityStillPresentVerified = $identityStillPresentThrew -and $freshPresentCensusCalls.Value -eq 1 -and
        $identityStillPresentObservation.trackedSweepFailureType -eq 'identity-still-present' -and
        $identityStillPresentObservation.trackedSweepFailureErrorCode -eq 5 -and
        $identityStillPresentObservation.trackedSweepDisappearedAfterSnapshotCount -eq 0 -and
        $identityStillPresentObservation.trackedSweepStillPresentAfterFailureCount -eq 1
    if (-not $identityStillPresentVerified) { throw 'Tracked identity-still-present self-test failed.' }

    $identityUnavailableObservation = New-StartupCleanupObservation
    $freshUnavailableCensusCalls = [pscustomobject]@{ Value = 0 }
    $identityUnavailableThrew = $false
    try {
        Invoke-StartupTrackedIdentityFailure $identityUnavailableObservation 1234 $identityFailureProbe {
            $freshUnavailableCensusCalls.Value++
            return [pscustomobject][ordered]@{
                Attempted = $true; AttemptCount = 1; Complete = $false; Succeeded = $false; ErrorCode = 24; RetryCount = 1; Retried = $true; Entries = $null
            }
        } 'Synthetic tracked identity failure.'
    }
    catch { $identityUnavailableThrew = $true }
    $identityUnavailableVerified = $identityUnavailableThrew -and $freshUnavailableCensusCalls.Value -eq 1 -and
        $identityUnavailableObservation.trackedSweepFailureType -eq 'enumeration-unavailable' -and
        $identityUnavailableObservation.trackedSweepFailureErrorCode -eq 5 -and
        $identityUnavailableObservation.trackedSweepDisappearedAfterSnapshotCount -eq 0 -and
        $identityUnavailableObservation.trackedSweepStillPresentAfterFailureCount -eq 0
    if (-not $identityUnavailableVerified) { throw 'Tracked enumeration-unavailable self-test failed.' }

    # The same recovery must work for the affinity path, which intentionally
    # calls the tracked sweep without an observation object.  It still gets
    # exactly one fresh census and accepts only a proven absence.
    $identityNoObservationCalls = [pscustomobject]@{ Value = 0 }
    $identityNoObservationAccepted = $false
    try {
        $identityNoObservationAccepted = [bool](Invoke-StartupTrackedIdentityFailure $null 1234 $identityFailureProbe {
            $identityNoObservationCalls.Value++
            return $freshAbsentCensus
        } 'Synthetic tracked identity failure.')
    }
    catch { $identityNoObservationAccepted = $false }
    $identityNoObservationVerified = $identityNoObservationAccepted -and $identityNoObservationCalls.Value -eq 1
    if (-not $identityNoObservationVerified) { throw 'Tracked identity-disappeared no-observation self-test failed.' }

    # Invocation exceptions, null probes, zero error codes, and contradictory
    # typed results are not coherent identity failures and must not trigger a
    # fresh-absence recovery.
    $identityNoFreshCalls = [pscustomobject]@{ Value = 0 }
    $identityExceptionThrew = $false
    try {
        Invoke-StartupTrackedIdentityFailure $null 1234 $null {
            $identityNoFreshCalls.Value++
            return $freshAbsentCensus
        } 'Synthetic identity invocation exception.'
    }
    catch { $identityExceptionThrew = $true }
    $identityZeroErrorThrew = $false
    try {
        Invoke-StartupTrackedIdentityFailure $null 1234 ([pscustomobject][ordered]@{ Succeeded = $false; ErrorCode = 0; Identity = $null }) {
            $identityNoFreshCalls.Value++
            return $freshAbsentCensus
        } 'Synthetic zero-error identity failure.'
    }
    catch { $identityZeroErrorThrew = $true }
    $identityContradictoryThrew = $false
    try {
        Invoke-StartupTrackedIdentityFailure $null 1234 ([pscustomobject][ordered]@{
            Succeeded = $true; ErrorCode = 0; Identity = [pscustomobject]@{ ProcessId = 1234 }
        }) {
            $identityNoFreshCalls.Value++
            return $freshAbsentCensus
        } 'Synthetic contradictory identity result.'
    }
    catch { $identityContradictoryThrew = $true }
    $identityMalformedProbeVerified = $identityExceptionThrew -and $identityZeroErrorThrew -and
        $identityContradictoryThrew -and $identityNoFreshCalls.Value -eq 0
    if (-not $identityMalformedProbeVerified) { throw 'Malformed tracked identity probe self-test failed.' }

    # A successful fresh census must validate every entry before it can prove
    # absence.  An incomplete result, duplicate PID, or malformed entry is a
    # terminal failure even when the target PID is not listed.
    $freshMalformedCalls = [pscustomobject]@{ Value = 0 }
    $freshTooManyEntries = [Array]::CreateInstance([object], 65537)
    $freshMalformedCases = @(
        [pscustomobject][ordered]@{ Attempted = $false; AttemptCount = 1; Complete = $true; Succeeded = $true; ErrorCode = 0; RetryCount = 0; Retried = $false; Entries = @() }
        [pscustomobject][ordered]@{ Attempted = $true; AttemptCount = 1; Complete = $true; Succeeded = $true; ErrorCode = 0; RetryCount = 0; Retried = $false; Entries = [object[]]@(
            [pscustomobject]@{ ProcessId = 2222; ParentProcessId = 0; ImageName = 'self-test.exe' }
            [pscustomobject]@{ ProcessId = 2222; ParentProcessId = 0; ImageName = 'self-test.exe' }
        ) }
        [pscustomobject][ordered]@{ Attempted = $true; AttemptCount = 1; Complete = $true; Succeeded = $true; ErrorCode = 0; RetryCount = 0; Retried = $false; Entries = [object[]]@(
            [pscustomobject]@{ ProcessId = 2222; ParentProcessId = 0; ImageName = '' }
        ) }
        [pscustomobject][ordered]@{ Attempted = $true; AttemptCount = 1; Complete = $true; Succeeded = $true; ErrorCode = 0; RetryCount = 0; Retried = $false; Entries = [pscustomobject]@{ ProcessId = 2222; ParentProcessId = 0; ImageName = 'self-test.exe' } }
        [pscustomobject][ordered]@{ Attempted = $true; AttemptCount = 0; Complete = $true; Succeeded = $true; ErrorCode = 0; RetryCount = 0; Retried = $false; Entries = @() }
        [pscustomobject][ordered]@{ Attempted = $true; AttemptCount = 4; Complete = $true; Succeeded = $true; ErrorCode = 0; RetryCount = 0; Retried = $false; Entries = @() }
        [pscustomobject][ordered]@{ Attempted = $true; AttemptCount = 1; Complete = $true; Succeeded = $true; ErrorCode = 0; RetryCount = -1; Retried = $false; Entries = @() }
        [pscustomobject][ordered]@{ Attempted = $true; AttemptCount = 1; Complete = $true; Succeeded = $true; ErrorCode = 0; RetryCount = 1; Retried = $true; Entries = @() }
        [pscustomobject][ordered]@{ Attempted = $true; AttemptCount = 1; Complete = $true; Succeeded = $true; ErrorCode = 0; RetryCount = 0; Retried = $false; Entries = $freshTooManyEntries }
        [pscustomobject][ordered]@{ Attempted = $true; AttemptCount = [double]1; Complete = $true; Succeeded = $true; ErrorCode = 0; RetryCount = 0; Retried = $false; Entries = @() }
        [pscustomobject][ordered]@{ Attempted = $true; AttemptCount = 1; Complete = $true; Succeeded = $true; ErrorCode = 0; RetryCount = [double]0; Retried = $false; Entries = @() }
        [pscustomobject][ordered]@{ Attempted = $true; AttemptCount = 1; Complete = $true; Succeeded = $true; ErrorCode = [double]0; RetryCount = 0; Retried = $false; Entries = @() }
        [pscustomobject][ordered]@{ Attempted = $true; AttemptCount = 1; Complete = $true; Succeeded = $true; ErrorCode = 0; RetryCount = 0; Entries = @() }
        [pscustomobject][ordered]@{ Attempted = $true; AttemptCount = 1; Complete = $true; Succeeded = $true; ErrorCode = 0; RetryCount = 0; Retried = 'false'; Entries = @() }
        [pscustomobject][ordered]@{ Attempted = $true; AttemptCount = 1; Complete = $true; Succeeded = $true; ErrorCode = 0; RetryCount = 0; Retried = $true; Entries = @() }
    )
    $freshMalformedRejected = $true
    foreach ($freshMalformed in $freshMalformedCases) {
        try {
            Invoke-StartupTrackedIdentityFailure $null 1234 $identityFailureProbe {
                $freshMalformedCalls.Value++
                return $freshMalformed
            } 'Synthetic malformed fresh census.'
            $freshMalformedRejected = $false
        }
        catch { }
    }
    $freshMalformedVerified = $freshMalformedRejected -and
        $freshMalformedCalls.Value -eq $freshMalformedCases.Count
    if (-not $freshMalformedVerified) { throw 'Malformed fresh census self-test failed.' }

    # Job membership identity gaps use the same one-shot recovery, but do not
    # emit tracked-sweep-specific telemetry.  The process-enumeration aggregate
    # still records the fresh census; every non-absence result remains a terminal failure.
    $jobIdentityDisappearedTelemetryDisabledObservation = New-StartupCleanupObservation
    $jobIdentityDisappearedTelemetryDisabledCalls = [pscustomobject]@{ Value = 0 }
    $jobIdentityDisappearedTelemetryDisabledAccepted = $false
    try {
        $jobIdentityDisappearedTelemetryDisabledAccepted = [bool](Invoke-StartupTrackedIdentityFailure `
            $jobIdentityDisappearedTelemetryDisabledObservation 1234 $identityFailureProbe {
                $jobIdentityDisappearedTelemetryDisabledCalls.Value++
                return $freshAbsentCensus
            } 'Synthetic Job identity disappearance.' -RecordTrackedSweepTelemetry:$false)
    }
    catch { $jobIdentityDisappearedTelemetryDisabledAccepted = $false }
    $jobIdentityDisappearedTelemetryDisabledVerified =
        $jobIdentityDisappearedTelemetryDisabledAccepted -and
        $jobIdentityDisappearedTelemetryDisabledCalls.Value -eq 1 -and
        $jobIdentityDisappearedTelemetryDisabledObservation.processEnumerationCallCount -eq 1 -and
        $jobIdentityDisappearedTelemetryDisabledObservation.processEnumerationCompletedCount -eq 1 -and
        $jobIdentityDisappearedTelemetryDisabledObservation.processEnumerationFailureCount -eq 0 -and
        $jobIdentityDisappearedTelemetryDisabledObservation.trackedSweepFailureType -eq 'none' -and
        $jobIdentityDisappearedTelemetryDisabledObservation.trackedSweepFailureErrorCode -eq $null -and
        $jobIdentityDisappearedTelemetryDisabledObservation.trackedSweepIdentityAttemptCount -eq 0 -and
        $jobIdentityDisappearedTelemetryDisabledObservation.trackedSweepIdentityFailureCount -eq 0 -and
        $jobIdentityDisappearedTelemetryDisabledObservation.trackedSweepDisappearedAfterSnapshotCount -eq 0 -and
        $jobIdentityDisappearedTelemetryDisabledObservation.trackedSweepStillPresentAfterFailureCount -eq 0 -and
        $jobIdentityDisappearedTelemetryDisabledObservation.trackedSweepPassCount -eq 0 -and
        $jobIdentityDisappearedTelemetryDisabledObservation.cleanupErrorCount -eq 0
    if (-not $jobIdentityDisappearedTelemetryDisabledVerified) {
        throw 'Job identity-disappeared telemetry suppression self-test failed.'
    }

    $jobIdentityStillPresentTelemetryDisabledObservation = New-StartupCleanupObservation
    $jobIdentityStillPresentTelemetryDisabledCalls = [pscustomobject]@{ Value = 0 }
    $jobIdentityStillPresentTelemetryDisabledThrew = $false
    try {
        Invoke-StartupTrackedIdentityFailure $jobIdentityStillPresentTelemetryDisabledObservation 1234 $identityFailureProbe {
            $jobIdentityStillPresentTelemetryDisabledCalls.Value++
            return $freshPresentCensus
        } 'Synthetic Job identity-still-present failure.' -RecordTrackedSweepTelemetry:$false
    }
    catch { $jobIdentityStillPresentTelemetryDisabledThrew = $true }
    $jobIdentityStillPresentTelemetryDisabledVerified =
        $jobIdentityStillPresentTelemetryDisabledThrew -and
        $jobIdentityStillPresentTelemetryDisabledCalls.Value -eq 1 -and
        $jobIdentityStillPresentTelemetryDisabledObservation.trackedSweepFailureType -eq 'none' -and
        $jobIdentityStillPresentTelemetryDisabledObservation.trackedSweepFailureErrorCode -eq $null -and
        $jobIdentityStillPresentTelemetryDisabledObservation.trackedSweepIdentityAttemptCount -eq 0 -and
        $jobIdentityStillPresentTelemetryDisabledObservation.trackedSweepIdentityFailureCount -eq 0 -and
        $jobIdentityStillPresentTelemetryDisabledObservation.trackedSweepDisappearedAfterSnapshotCount -eq 0 -and
        $jobIdentityStillPresentTelemetryDisabledObservation.trackedSweepStillPresentAfterFailureCount -eq 0 -and
        $jobIdentityStillPresentTelemetryDisabledObservation.trackedSweepPassCount -eq 0
    if (-not $jobIdentityStillPresentTelemetryDisabledVerified) {
        throw 'Job identity-still-present telemetry suppression self-test failed.'
    }

    $jobIdentityCensusUnavailableTelemetryDisabledObservation = New-StartupCleanupObservation
    $jobIdentityCensusUnavailableTelemetryDisabledCalls = [pscustomobject]@{ Value = 0 }
    $jobIdentityCensusUnavailableTelemetryDisabledThrew = $false
    try {
        Invoke-StartupTrackedIdentityFailure $jobIdentityCensusUnavailableTelemetryDisabledObservation 1234 $identityFailureProbe {
            $jobIdentityCensusUnavailableTelemetryDisabledCalls.Value++
            return [pscustomobject][ordered]@{
                Attempted = $true; AttemptCount = 1; Complete = $false; Succeeded = $false; ErrorCode = 24; RetryCount = 0; Entries = $null
            }
        } 'Synthetic unavailable Job identity census.' -RecordTrackedSweepTelemetry:$false
    }
    catch { $jobIdentityCensusUnavailableTelemetryDisabledThrew = $true }
    $jobIdentityCensusUnavailableTelemetryDisabledVerified =
        $jobIdentityCensusUnavailableTelemetryDisabledThrew -and
        $jobIdentityCensusUnavailableTelemetryDisabledCalls.Value -eq 1 -and
        $jobIdentityCensusUnavailableTelemetryDisabledObservation.processEnumerationFailureCount -eq 1 -and
        $jobIdentityCensusUnavailableTelemetryDisabledObservation.trackedSweepFailureType -eq 'none' -and
        $jobIdentityCensusUnavailableTelemetryDisabledObservation.trackedSweepFailureErrorCode -eq $null -and
        $jobIdentityCensusUnavailableTelemetryDisabledObservation.trackedSweepIdentityAttemptCount -eq 0 -and
        $jobIdentityCensusUnavailableTelemetryDisabledObservation.trackedSweepIdentityFailureCount -eq 0 -and
        $jobIdentityCensusUnavailableTelemetryDisabledObservation.trackedSweepDisappearedAfterSnapshotCount -eq 0 -and
        $jobIdentityCensusUnavailableTelemetryDisabledObservation.trackedSweepStillPresentAfterFailureCount -eq 0 -and
        $jobIdentityCensusUnavailableTelemetryDisabledObservation.trackedSweepPassCount -eq 0
    if (-not $jobIdentityCensusUnavailableTelemetryDisabledVerified) {
        throw 'Job unavailable identity census telemetry suppression self-test failed.'
    }

    $jobIdentityMalformedFreshCensusTelemetryDisabledObservation = New-StartupCleanupObservation
    $jobIdentityMalformedFreshCensusTelemetryDisabledCalls = [pscustomobject]@{ Value = 0 }
    $jobIdentityMalformedFreshCensusTelemetryDisabledThrew = $false
    try {
        Invoke-StartupTrackedIdentityFailure $jobIdentityMalformedFreshCensusTelemetryDisabledObservation 1234 $identityFailureProbe {
            $jobIdentityMalformedFreshCensusTelemetryDisabledCalls.Value++
            return [pscustomobject][ordered]@{
                Attempted = $true; AttemptCount = 1; Complete = $true; Succeeded = $true; ErrorCode = 0; RetryCount = 0
                Entries = [object[]]@(
                    [pscustomobject]@{ ProcessId = 2222; ParentProcessId = 0; ImageName = 'self-test.exe' }
                    [pscustomobject]@{ ProcessId = 2222; ParentProcessId = 0; ImageName = 'self-test.exe' }
                )
            }
        } 'Synthetic malformed Job identity census.' -RecordTrackedSweepTelemetry:$false
    }
    catch { $jobIdentityMalformedFreshCensusTelemetryDisabledThrew = $true }
    $jobIdentityMalformedFreshCensusTelemetryDisabledVerified =
        $jobIdentityMalformedFreshCensusTelemetryDisabledThrew -and
        $jobIdentityMalformedFreshCensusTelemetryDisabledCalls.Value -eq 1 -and
        $jobIdentityMalformedFreshCensusTelemetryDisabledObservation.processEnumerationCompletedCount -eq 1 -and
        $jobIdentityMalformedFreshCensusTelemetryDisabledObservation.processEnumerationFailureCount -eq 0 -and
        $jobIdentityMalformedFreshCensusTelemetryDisabledObservation.trackedSweepFailureType -eq 'none' -and
        $jobIdentityMalformedFreshCensusTelemetryDisabledObservation.trackedSweepFailureErrorCode -eq $null -and
        $jobIdentityMalformedFreshCensusTelemetryDisabledObservation.trackedSweepIdentityAttemptCount -eq 0 -and
        $jobIdentityMalformedFreshCensusTelemetryDisabledObservation.trackedSweepIdentityFailureCount -eq 0 -and
        $jobIdentityMalformedFreshCensusTelemetryDisabledObservation.trackedSweepDisappearedAfterSnapshotCount -eq 0 -and
        $jobIdentityMalformedFreshCensusTelemetryDisabledObservation.trackedSweepStillPresentAfterFailureCount -eq 0 -and
        $jobIdentityMalformedFreshCensusTelemetryDisabledObservation.trackedSweepPassCount -eq 0
    if (-not $jobIdentityMalformedFreshCensusTelemetryDisabledVerified) {
        throw 'Job malformed identity census telemetry suppression self-test failed.'
    }
    $jobIdentityTelemetryDisabledSelfTestVerified =
        $jobIdentityDisappearedTelemetryDisabledVerified -and
        $jobIdentityStillPresentTelemetryDisabledVerified -and
        $jobIdentityCensusUnavailableTelemetryDisabledVerified -and
        $jobIdentityMalformedFreshCensusTelemetryDisabledVerified

    # Exercise the production Get-JobProcessRecords path with a sentinel handle
    # and injected native-shaped invokers.  The initial census always contains
    # the target PID, so this cannot accidentally test the separate missing-
    # entry Job requery path.
    $jobIdentityTargetPid = 1234
    $jobIdentityLaterPid = 2345
    $makeJobIdentityQuery = {
        param([int[]]$Ids = [int[]]@(), [bool]$Succeeded = $true, [int]$ErrorCode = 0)
        $normalizedIds = if ($null -eq $Ids) { [int[]]@() } else { ,([int[]]$Ids) }
        $listedCount = if ($Succeeded) { [UInt32](@($normalizedIds).Count) } else { [UInt32]0 }
        $attempt = [pscustomobject][ordered]@{
            Attempted = $true; AttemptNumber = 1; Succeeded = [bool]$Succeeded
            ErrorCode = if ($Succeeded) { 0 } else { [int]$ErrorCode }
            CapacityBytes = [UInt64]1024; RequiredBytes = [UInt64]1024; ReturnLengthBytes = [UInt64]1024
            AssignedProcessCount = $listedCount; ListedProcessCount = $listedCount; Resized = $false
        }
        return [pscustomobject][ordered]@{
            Handle = [IntPtr]1; Succeeded = [bool]$Succeeded; ErrorCode = if ($Succeeded) { 0 } else { [int]$ErrorCode }
            ProcessIds = $normalizedIds; Attempted = $true; AttemptCount = 1
            CapacityBytes = [UInt64]1024; RequiredBytes = [UInt64]1024; ReturnLengthBytes = [UInt64]1024
            AssignedProcessCount = $listedCount; ListedProcessCount = $listedCount; Resized = $false
            Attempts = @($attempt)
        }
    }
    $makeJobIdentityCensus = {
        param([object[]]$Entries, [bool]$Complete = $true, [bool]$Succeeded = $true, [int]$ErrorCode = 0)
        return [pscustomobject][ordered]@{
            Attempted = $true; AttemptCount = 1; Complete = [bool]$Complete; Succeeded = [bool]$Succeeded
            ErrorCode = if ($Succeeded) { 0 } else { [int]$ErrorCode }; RetryCount = 0; Retried = $false
            Entries = if ($null -eq $Entries) { $null } else { ,([object[]]$Entries) }
        }
    }
    $jobIdentityTargetEntry = [pscustomobject]@{ ProcessId = $jobIdentityTargetPid; ParentProcessId = 0; ImageName = 'self-test.exe' }
    $jobIdentityLaterEntry = [pscustomobject]@{ ProcessId = $jobIdentityLaterPid; ParentProcessId = $jobIdentityTargetPid; ImageName = 'self-test.exe' }
    $jobIdentityTargetRecord = [pscustomobject]@{
        ProcessId = $jobIdentityTargetPid; ParentProcessId = 0; CreationTime = [long]1001; ImagePath = 'C:\self-test.exe'
    }
    $jobIdentityLaterRecord = [pscustomobject]@{
        ProcessId = $jobIdentityLaterPid; ParentProcessId = $jobIdentityTargetPid; CreationTime = [long]1002; ImagePath = 'C:\self-test.exe'
    }
    $jobIdentityLaterIdentityProbe = [pscustomobject][ordered]@{
        Succeeded = $true; ErrorCode = 0; Identity = $jobIdentityLaterRecord
    }
    $jobIdentityFailureProbe = [pscustomobject][ordered]@{ Succeeded = $false; ErrorCode = 5; Identity = $null }
    $jobIdentityInitialQuery = & $makeJobIdentityQuery ([int[]]@($jobIdentityTargetPid, $jobIdentityLaterPid))
    $jobIdentityLaterQuery = & $makeJobIdentityQuery ([int[]]@($jobIdentityLaterPid))
    $jobIdentityTargetQuery = & $makeJobIdentityQuery ([int[]]@($jobIdentityTargetPid))
    $jobIdentityInitialCensus = & $makeJobIdentityCensus ([object[]]@($jobIdentityTargetEntry, $jobIdentityLaterEntry))
    $jobIdentityLaterCensus = & $makeJobIdentityCensus ([object[]]@($jobIdentityLaterEntry))
    $jobIdentityTargetCensus = & $makeJobIdentityCensus ([object[]]@($jobIdentityTargetEntry))

    $jobIdentityAcceptedObservation = New-StartupCleanupObservation
    $jobIdentityAcceptedQueryObservation = New-StartupJobQueryObservation
    $jobIdentityAcceptedQueryObservation['jobIdentityObservation'] = $jobIdentityAcceptedObservation.jobIdentityObservation
    $jobIdentityAcceptedOwned = @{}
    $jobIdentityAcceptedQueryCalls = [pscustomobject]@{ Value = 0 }
    $jobIdentityAcceptedCensusCalls = [pscustomobject]@{ Value = 0 }
    $jobIdentityAcceptedIdentityCalls = [pscustomobject]@{ Value = 0 }
    $jobIdentityAcceptedJobQuery = {
        param([IntPtr]$Handle)
        $jobIdentityAcceptedQueryCalls.Value++
        if ($jobIdentityAcceptedQueryCalls.Value -eq 1) { return $jobIdentityInitialQuery }
        return $jobIdentityLaterQuery
    }
    $jobIdentityAcceptedCensus = {
        $jobIdentityAcceptedCensusCalls.Value++
        if ($jobIdentityAcceptedCensusCalls.Value -eq 1) { return $jobIdentityInitialCensus }
        return $jobIdentityLaterCensus
    }
    $jobIdentityAcceptedIdentity = {
        param([int]$ProcessId, [int]$ParentProcessId)
        $jobIdentityAcceptedIdentityCalls.Value++
        if ($ProcessId -eq $jobIdentityTargetPid) { return $jobIdentityFailureProbe }
        return $jobIdentityLaterIdentityProbe
    }
    $jobIdentityAcceptedInvokers = [pscustomobject][ordered]@{
        JobQuery = $jobIdentityAcceptedJobQuery; ProcessCensus = $jobIdentityAcceptedCensus; IdentityQuery = $jobIdentityAcceptedIdentity
    }
    $jobIdentityAcceptedRecords = @()
    $jobIdentityAcceptedThrew = $false
    try {
        $jobIdentityAcceptedRecords = @(Get-JobProcessRecords ([IntPtr]1) $jobIdentityAcceptedOwned $jobIdentityAcceptedQueryObservation $null $jobIdentityAcceptedInvokers $jobIdentityAcceptedObservation.jobIdentityObservation)
    }
    catch { $jobIdentityAcceptedThrew = $true }
    $jobIdentityAcceptedTelemetry = $jobIdentityAcceptedObservation.jobIdentityObservation
    $jobIdentityAcceptedTrackedClean = $jobIdentityAcceptedObservation.trackedSweepFailureType -eq 'none' -and
        $jobIdentityAcceptedObservation.trackedSweepFailureErrorCode -eq $null -and
        $jobIdentityAcceptedObservation.trackedSweepIdentityAttemptCount -eq 0 -and
        $jobIdentityAcceptedObservation.trackedSweepIdentityFailureCount -eq 0 -and
        $jobIdentityAcceptedObservation.trackedSweepDisappearedAfterSnapshotCount -eq 0 -and
        $jobIdentityAcceptedObservation.trackedSweepStillPresentAfterFailureCount -eq 0 -and
        $jobIdentityAcceptedObservation.trackedSweepPassCount -eq 0
    $jobIdentityAcceptedVerified = -not $jobIdentityAcceptedThrew -and $jobIdentityAcceptedRecords.Count -eq 1 -and
        $jobIdentityAcceptedRecords[0].Id -eq $jobIdentityLaterPid -and $jobIdentityAcceptedOwned.ContainsKey($jobIdentityLaterPid) -and
        $jobIdentityAcceptedQueryCalls.Value -eq 2 -and $jobIdentityAcceptedCensusCalls.Value -eq 1 -and
        $jobIdentityAcceptedIdentityCalls.Value -eq 2 -and $jobIdentityAcceptedQueryObservation.queryCount -eq 2 -and
        $jobIdentityAcceptedObservation.cleanupErrorCount -eq 0 -and $jobIdentityAcceptedTrackedClean -and
        $jobIdentityAcceptedTelemetry.identityAttemptCount -eq 2 -and
        $jobIdentityAcceptedTelemetry.identityFailureCount -eq 1 -and
        $jobIdentityAcceptedTelemetry.recoveryAttemptCount -eq 1 -and
        $jobIdentityAcceptedTelemetry.disappearedAfterSnapshotCount -eq 1 -and
        $jobIdentityAcceptedTelemetry.stillPresentAfterFailureCount -eq 0 -and
        $jobIdentityAcceptedTelemetry.failureType -eq 'identity-disappeared' -and
        $jobIdentityAcceptedTelemetry.failureErrorCode -eq 5
    if (-not $jobIdentityAcceptedVerified) { throw 'Production Job identity-disappeared self-test failed.' }

    $jobIdentityStillPresentObservation = New-StartupCleanupObservation
    $jobIdentityStillPresentQueryCalls = [pscustomobject]@{ Value = 0 }
    $jobIdentityStillPresentCensusCalls = [pscustomobject]@{ Value = 0 }
    $jobIdentityStillPresentIdentityCalls = [pscustomobject]@{ Value = 0 }
    $jobIdentityStillPresentJobQuery = {
        param([IntPtr]$Handle)
        $jobIdentityStillPresentQueryCalls.Value++
        return $jobIdentityTargetQuery
    }
    $jobIdentityStillPresentCensus = {
        $jobIdentityStillPresentCensusCalls.Value++
        return $jobIdentityTargetCensus
    }
    $jobIdentityStillPresentIdentity = {
        param([int]$ProcessId, [int]$ParentProcessId)
        $jobIdentityStillPresentIdentityCalls.Value++
        return $jobIdentityFailureProbe
    }
    $jobIdentityStillPresentInvokers = [pscustomobject][ordered]@{
        JobQuery = $jobIdentityStillPresentJobQuery; ProcessCensus = $jobIdentityStillPresentCensus; IdentityQuery = $jobIdentityStillPresentIdentity
    }
    $jobIdentityStillPresentThrew = $false
    try { [void](Get-JobProcessRecords ([IntPtr]1) @{} $jobIdentityStillPresentObservation $jobIdentityStillPresentObservation $jobIdentityStillPresentInvokers) }
    catch { $jobIdentityStillPresentThrew = $true }
    $jobIdentityStillPresentTelemetry = $jobIdentityStillPresentObservation.jobIdentityObservation
    $jobIdentityStillPresentVerified = $jobIdentityStillPresentThrew -and
        $jobIdentityStillPresentQueryCalls.Value -eq 2 -and $jobIdentityStillPresentCensusCalls.Value -eq 1 -and
        $jobIdentityStillPresentIdentityCalls.Value -eq 1 -and $jobIdentityStillPresentObservation.query.queryCount -eq 2 -and
        $jobIdentityStillPresentObservation.cleanupErrorCount -eq 0 -and
        $jobIdentityStillPresentTelemetry.identityAttemptCount -eq 1 -and
        $jobIdentityStillPresentTelemetry.identityFailureCount -eq 1 -and
        $jobIdentityStillPresentTelemetry.recoveryAttemptCount -eq 1 -and
        $jobIdentityStillPresentTelemetry.disappearedAfterSnapshotCount -eq 0 -and
        $jobIdentityStillPresentTelemetry.stillPresentAfterFailureCount -eq 1 -and
        $jobIdentityStillPresentTelemetry.failureType -eq 'identity-still-present' -and
        $jobIdentityStillPresentTelemetry.failureErrorCode -eq 5 -and
        $jobIdentityStillPresentObservation.trackedSweepFailureType -eq 'none'
    if (-not $jobIdentityStillPresentVerified) { throw 'Production Job identity-still-present self-test failed.' }

    # The resolver above must keep throwing.  Only the optional graceful-close
    # coordinator may consume the exact counter transition and transfer control
    # to the kill-on-close Job.  Preserve an earlier first cause to mirror the
    # measured Rust run (disappearance first, still-present later).
    $gracefulFallbackObservation = New-StartupCleanupObservation
    Set-StartupJobIdentityFailure $gracefulFallbackObservation.jobIdentityObservation 'identity-disappeared' 31
    $gracefulFallbackObservation.jobIdentityObservation.identityAttemptCount = 2
    $gracefulFallbackObservation.jobIdentityObservation.identityFailureCount = 2
    $gracefulFallbackObservation.jobIdentityObservation.recoveryAttemptCount = 2
    $gracefulFallbackObservation.jobIdentityObservation.disappearedAfterSnapshotCount = 1
    $gracefulFallbackObservation.jobIdentityObservation.stillPresentAfterFailureCount = 0
    $gracefulStillPresentBefore = [int]$gracefulFallbackObservation.jobIdentityObservation.stillPresentAfterFailureCount
    $gracefulFallbackObservation.jobIdentityObservation.stillPresentAfterFailureCount = 1
    $gracefulFallbackAccepted = Try-StartupGracefulJobIdentityFallback `
        $gracefulFallbackObservation $gracefulStillPresentBefore
    $gracefulFallbackRepeated = Try-StartupGracefulJobIdentityFallback `
        $gracefulFallbackObservation $gracefulStillPresentBefore
    $gracefulNoDeltaObservation = New-StartupCleanupObservation
    $gracefulNoDeltaAccepted = Try-StartupGracefulJobIdentityFallback $gracefulNoDeltaObservation 0
    $gracefulMalformedObservation = New-StartupCleanupObservation
    $gracefulMalformedObservation.jobIdentityObservation.stillPresentAfterFailureCount = '1'
    $gracefulMalformedAccepted = Try-StartupGracefulJobIdentityFallback $gracefulMalformedObservation 0
    $gracefulIdentityFallbackSelfTestVerified = $gracefulFallbackAccepted -and
        -not $gracefulFallbackRepeated -and -not $gracefulNoDeltaAccepted -and
        -not $gracefulMalformedAccepted -and
        $gracefulFallbackObservation.gracefulCloseFallbackType -eq 'identity-still-present' -and
        $gracefulFallbackObservation.jobIdentityObservation.failureType -eq 'identity-disappeared' -and
        $gracefulFallbackObservation.jobIdentityObservation.failureErrorCode -eq 31 -and
        $gracefulNoDeltaObservation.gracefulCloseFallbackType -eq 'none' -and
        $gracefulMalformedObservation.gracefulCloseFallbackType -eq 'none'
    if (-not $gracefulIdentityFallbackSelfTestVerified) {
        throw 'Graceful Job identity fallback self-test failed.'
    }

    $jobIdentityUnavailableObservation = New-StartupCleanupObservation
    $jobIdentityUnavailableQueryCalls = [pscustomobject]@{ Value = 0 }
    $jobIdentityUnavailableCensusCalls = [pscustomobject]@{ Value = 0 }
    $jobIdentityUnavailableJobQuery = {
        param([IntPtr]$Handle)
        $jobIdentityUnavailableQueryCalls.Value++
        if ($jobIdentityUnavailableQueryCalls.Value -eq 1) { return $jobIdentityTargetQuery }
        return (& $makeJobIdentityQuery ([int[]]@()) $false 24)
    }
    $jobIdentityUnavailableCensus = {
        $jobIdentityUnavailableCensusCalls.Value++
        return $jobIdentityTargetCensus
    }
    $jobIdentityUnavailableInvokers = [pscustomobject][ordered]@{
        JobQuery = $jobIdentityUnavailableJobQuery; ProcessCensus = $jobIdentityUnavailableCensus
        IdentityQuery = { param([int]$ProcessId, [int]$ParentProcessId) return $jobIdentityFailureProbe }
    }
    $jobIdentityUnavailableThrew = $false
    try { [void](Get-JobProcessRecords ([IntPtr]1) @{} $jobIdentityUnavailableObservation $jobIdentityUnavailableObservation $jobIdentityUnavailableInvokers) }
    catch { $jobIdentityUnavailableThrew = $true }
    $jobIdentityUnavailableTelemetry = $jobIdentityUnavailableObservation.jobIdentityObservation
    $jobIdentityUnavailableVerified = $jobIdentityUnavailableThrew -and
        $jobIdentityUnavailableQueryCalls.Value -eq 2 -and $jobIdentityUnavailableCensusCalls.Value -eq 1 -and
        $jobIdentityUnavailableObservation.query.queryCount -eq 2 -and
        $jobIdentityUnavailableTelemetry.identityAttemptCount -eq 1 -and
        $jobIdentityUnavailableTelemetry.identityFailureCount -eq 1 -and
        $jobIdentityUnavailableTelemetry.recoveryAttemptCount -eq 1 -and
        $jobIdentityUnavailableTelemetry.disappearedAfterSnapshotCount -eq 0 -and
        $jobIdentityUnavailableTelemetry.stillPresentAfterFailureCount -eq 0 -and
        $jobIdentityUnavailableTelemetry.failureType -eq 'enumeration-unavailable' -and
        $jobIdentityUnavailableTelemetry.failureErrorCode -eq 24 -and
        $jobIdentityUnavailableObservation.trackedSweepFailureType -eq 'none'
    if (-not $jobIdentityUnavailableVerified) { throw 'Production Job unavailable identity self-test failed.' }

    $jobIdentityMalformedObservation = New-StartupCleanupObservation
    $jobIdentityMalformedQueryCalls = [pscustomobject]@{ Value = 0 }
    $jobIdentityMalformedCensusCalls = [pscustomobject]@{ Value = 0 }
    $jobIdentityMalformedJobQuery = {
        param([IntPtr]$Handle)
        $jobIdentityMalformedQueryCalls.Value++
        if ($jobIdentityMalformedQueryCalls.Value -eq 1) { return $jobIdentityTargetQuery }
        return (& $makeJobIdentityQuery ([int[]]@($jobIdentityLaterPid, $jobIdentityLaterPid)) $true 0)
    }
    $jobIdentityMalformedCensus = {
        $jobIdentityMalformedCensusCalls.Value++
        return $jobIdentityTargetCensus
    }
    $jobIdentityMalformedInvokers = [pscustomobject][ordered]@{
        JobQuery = $jobIdentityMalformedJobQuery; ProcessCensus = $jobIdentityMalformedCensus
        IdentityQuery = { param([int]$ProcessId, [int]$ParentProcessId) return $jobIdentityFailureProbe }
    }
    $jobIdentityMalformedThrew = $false
    try { [void](Get-JobProcessRecords ([IntPtr]1) @{} $jobIdentityMalformedObservation $jobIdentityMalformedObservation $jobIdentityMalformedInvokers) }
    catch { $jobIdentityMalformedThrew = $true }
    $jobIdentityMalformedTelemetry = $jobIdentityMalformedObservation.jobIdentityObservation
    $jobIdentityMalformedVerified = $jobIdentityMalformedThrew -and
        $jobIdentityMalformedQueryCalls.Value -eq 2 -and $jobIdentityMalformedCensusCalls.Value -eq 1 -and
        $jobIdentityMalformedObservation.query.queryCount -eq 2 -and
        $jobIdentityMalformedTelemetry.identityAttemptCount -eq 1 -and
        $jobIdentityMalformedTelemetry.identityFailureCount -eq 1 -and
        $jobIdentityMalformedTelemetry.recoveryAttemptCount -eq 1 -and
        $jobIdentityMalformedTelemetry.failureType -eq 'enumeration-unavailable' -and
        $jobIdentityMalformedTelemetry.failureErrorCode -eq 13 -and
        $jobIdentityMalformedObservation.trackedSweepFailureType -eq 'none'
    if (-not $jobIdentityMalformedVerified) { throw 'Production Job malformed fresh-query self-test failed.' }

    $jobIdentityInvocationExceptionObservation = New-StartupCleanupObservation
    $jobIdentityInvocationExceptionQueryCalls = [pscustomobject]@{ Value = 0 }
    $jobIdentityInvocationExceptionCensusCalls = [pscustomobject]@{ Value = 0 }
    $jobIdentityInvocationExceptionJobQuery = {
        param([IntPtr]$Handle)
        $jobIdentityInvocationExceptionQueryCalls.Value++
        return $jobIdentityTargetQuery
    }
    $jobIdentityInvocationExceptionCensus = {
        $jobIdentityInvocationExceptionCensusCalls.Value++
        return $jobIdentityTargetCensus
    }
    $jobIdentityInvocationExceptionInvokers = [pscustomobject][ordered]@{
        JobQuery = $jobIdentityInvocationExceptionJobQuery; ProcessCensus = $jobIdentityInvocationExceptionCensus
        IdentityQuery = { param([int]$ProcessId, [int]$ParentProcessId) throw 'synthetic identity invocation exception' }
    }
    $jobIdentityInvocationExceptionThrew = $false
    try { [void](Get-JobProcessRecords ([IntPtr]1) @{} $jobIdentityInvocationExceptionObservation $jobIdentityInvocationExceptionObservation $jobIdentityInvocationExceptionInvokers) }
    catch { $jobIdentityInvocationExceptionThrew = $true }
    $jobIdentityInvocationExceptionTelemetry = $jobIdentityInvocationExceptionObservation.jobIdentityObservation
    $jobIdentityInvocationExceptionVerified = $jobIdentityInvocationExceptionThrew -and
        $jobIdentityInvocationExceptionQueryCalls.Value -eq 1 -and $jobIdentityInvocationExceptionCensusCalls.Value -eq 1 -and
        $jobIdentityInvocationExceptionTelemetry.identityAttemptCount -eq 1 -and
        $jobIdentityInvocationExceptionTelemetry.identityFailureCount -eq 1 -and
        $jobIdentityInvocationExceptionTelemetry.recoveryAttemptCount -eq 0 -and
        $jobIdentityInvocationExceptionTelemetry.failureType -eq 'exception' -and
        $jobIdentityInvocationExceptionTelemetry.failureErrorCode -eq 13 -and
        $jobIdentityInvocationExceptionObservation.trackedSweepFailureType -eq 'none'
    if (-not $jobIdentityInvocationExceptionVerified) { throw 'Production Job identity invocation exception self-test failed.' }

    $jobIdentityFreshQueryExceptionObservation = New-StartupCleanupObservation
    $jobIdentityFreshQueryExceptionQueryCalls = [pscustomobject]@{ Value = 0 }
    $jobIdentityFreshQueryExceptionCensusCalls = [pscustomobject]@{ Value = 0 }
    $jobIdentityFreshQueryExceptionJobQuery = {
        param([IntPtr]$Handle)
        $jobIdentityFreshQueryExceptionQueryCalls.Value++
        if ($jobIdentityFreshQueryExceptionQueryCalls.Value -eq 1) { return $jobIdentityTargetQuery }
        throw 'synthetic fresh Job query exception'
    }
    $jobIdentityFreshQueryExceptionCensus = {
        $jobIdentityFreshQueryExceptionCensusCalls.Value++
        return $jobIdentityTargetCensus
    }
    $jobIdentityFreshQueryExceptionInvokers = [pscustomobject][ordered]@{
        JobQuery = $jobIdentityFreshQueryExceptionJobQuery; ProcessCensus = $jobIdentityFreshQueryExceptionCensus
        IdentityQuery = { param([int]$ProcessId, [int]$ParentProcessId) return $jobIdentityFailureProbe }
    }
    $jobIdentityFreshQueryExceptionThrew = $false
    try { [void](Get-JobProcessRecords ([IntPtr]1) @{} $jobIdentityFreshQueryExceptionObservation $jobIdentityFreshQueryExceptionObservation $jobIdentityFreshQueryExceptionInvokers) }
    catch { $jobIdentityFreshQueryExceptionThrew = $true }
    $jobIdentityFreshQueryExceptionTelemetry = $jobIdentityFreshQueryExceptionObservation.jobIdentityObservation
    $jobIdentityFreshQueryExceptionVerified = $jobIdentityFreshQueryExceptionThrew -and
        $jobIdentityFreshQueryExceptionQueryCalls.Value -eq 2 -and $jobIdentityFreshQueryExceptionCensusCalls.Value -eq 1 -and
        $jobIdentityFreshQueryExceptionObservation.query.queryCount -eq 2 -and
        $jobIdentityFreshQueryExceptionTelemetry.identityAttemptCount -eq 1 -and
        $jobIdentityFreshQueryExceptionTelemetry.identityFailureCount -eq 1 -and
        $jobIdentityFreshQueryExceptionTelemetry.recoveryAttemptCount -eq 1 -and
        $jobIdentityFreshQueryExceptionTelemetry.failureType -eq 'exception' -and
        $jobIdentityFreshQueryExceptionTelemetry.failureErrorCode -eq 13 -and
        $jobIdentityFreshQueryExceptionObservation.trackedSweepFailureType -eq 'none'
    if (-not $jobIdentityFreshQueryExceptionVerified) { throw 'Production Job fresh-query exception self-test failed.' }

    $jobIdentityConversionFailureObservation = New-StartupCleanupObservation
    $jobIdentityConversionFailureQueryCalls = [pscustomobject]@{ Value = 0 }
    $jobIdentityConversionFailureCensusCalls = [pscustomobject]@{ Value = 0 }
    $jobIdentityConversionFailureJobQuery = {
        param([IntPtr]$Handle)
        $jobIdentityConversionFailureQueryCalls.Value++
        return $jobIdentityTargetQuery
    }
    $jobIdentityConversionFailureCensus = {
        $jobIdentityConversionFailureCensusCalls.Value++
        return $jobIdentityTargetCensus
    }
    $jobIdentityConversionFailureInvokers = [pscustomobject][ordered]@{
        JobQuery = $jobIdentityConversionFailureJobQuery; ProcessCensus = $jobIdentityConversionFailureCensus
        IdentityQuery = {
            param([int]$ProcessId, [int]$ParentProcessId)
            return [pscustomobject][ordered]@{
                Succeeded = $true; ErrorCode = 0
                Identity = [pscustomobject]@{ ProcessId = 'not-a-pid'; ParentProcessId = 0; CreationTime = 1001; ImagePath = 'C:\self-test.exe' }
            }
        }
    }
    $jobIdentityConversionFailureThrew = $false
    try { [void](Get-JobProcessRecords ([IntPtr]1) @{} $jobIdentityConversionFailureObservation $jobIdentityConversionFailureObservation $jobIdentityConversionFailureInvokers) }
    catch { $jobIdentityConversionFailureThrew = $true }
    $jobIdentityConversionFailureTelemetry = $jobIdentityConversionFailureObservation.jobIdentityObservation
    $jobIdentityConversionFailureVerified = $jobIdentityConversionFailureThrew -and
        $jobIdentityConversionFailureQueryCalls.Value -eq 1 -and $jobIdentityConversionFailureCensusCalls.Value -eq 1 -and
        $jobIdentityConversionFailureTelemetry.identityAttemptCount -eq 1 -and
        $jobIdentityConversionFailureTelemetry.identityFailureCount -eq 1 -and
        $jobIdentityConversionFailureTelemetry.recoveryAttemptCount -eq 0 -and
        $jobIdentityConversionFailureTelemetry.failureType -eq 'exception' -and
        $jobIdentityConversionFailureTelemetry.failureErrorCode -eq 13 -and
        $jobIdentityConversionFailureObservation.trackedSweepFailureType -eq 'none'
    if (-not $jobIdentityConversionFailureVerified) { throw 'Production Job identity conversion failure self-test failed.' }

    # A contradictory identity envelope is not a coherent disappearance.  It
    # must fail closed without attempting the fresh Job-membership recovery,
    # even when the payload carries an otherwise valid identity record.
    $jobIdentityContradictorySuccessCases = @(
        [pscustomobject][ordered]@{
            Probe = [pscustomobject][ordered]@{ Succeeded = $true; ErrorCode = 5; Identity = $jobIdentityTargetRecord }
            ExpectedErrorCode = 5
        }
        [pscustomobject][ordered]@{
            Probe = [pscustomobject][ordered]@{ Succeeded = $true; ErrorCode = 0; Identity = $null }
            ExpectedErrorCode = 13
        }
    )
    $jobIdentityContradictorySuccessRejected = $true
    foreach ($contradictoryCase in $jobIdentityContradictorySuccessCases) {
        $contradictoryObservation = New-StartupCleanupObservation
        $contradictoryQueryCalls = [pscustomobject]@{ Value = 0 }
        $contradictoryCensusCalls = [pscustomobject]@{ Value = 0 }
        $contradictoryIdentityCalls = [pscustomobject]@{ Value = 0 }
        $contradictoryInvokers = [pscustomobject][ordered]@{
            JobQuery = {
                param([IntPtr]$Handle)
                $contradictoryQueryCalls.Value++
                return $jobIdentityTargetQuery
            }
            ProcessCensus = {
                $contradictoryCensusCalls.Value++
                return $jobIdentityTargetCensus
            }
            IdentityQuery = {
                param([int]$ProcessId, [int]$ParentProcessId)
                $contradictoryIdentityCalls.Value++
                return $contradictoryCase.Probe
            }
        }
        $contradictoryThrew = $false
        try {
            [void](Get-JobProcessRecords ([IntPtr]1) @{} $contradictoryObservation $contradictoryObservation $contradictoryInvokers)
        }
        catch { $contradictoryThrew = $true }
        $contradictoryTelemetry = $contradictoryObservation.jobIdentityObservation
        if (-not ($contradictoryThrew -and $contradictoryQueryCalls.Value -eq 1 -and
                $contradictoryCensusCalls.Value -eq 1 -and $contradictoryIdentityCalls.Value -eq 1 -and
                $contradictoryTelemetry.identityAttemptCount -eq 1 -and
                $contradictoryTelemetry.identityFailureCount -eq 1 -and
                $contradictoryTelemetry.recoveryAttemptCount -eq 0 -and
                $contradictoryTelemetry.disappearedAfterSnapshotCount -eq 0 -and
                $contradictoryTelemetry.stillPresentAfterFailureCount -eq 0 -and
                $contradictoryTelemetry.failureType -eq 'identity-unavailable' -and
                $contradictoryTelemetry.failureErrorCode -eq $contradictoryCase.ExpectedErrorCode -and
                $contradictoryObservation.trackedSweepFailureType -eq 'none')) {
            $jobIdentityContradictorySuccessRejected = $false
        }
    }
    $jobIdentityContradictorySuccessVerified = [bool]$jobIdentityContradictorySuccessRejected
    if (-not $jobIdentityContradictorySuccessVerified) {
        throw 'Contradictory Job identity success envelope self-test failed.'
    }

    # Fresh Job-membership results are independently untrusted.  Keep the
    # initial census valid and containing the target, then reject each malformed
    # fresh query before it can qualify an identity disappearance.
    $makeMalformedFreshJobQuery = {
        param([AllowNull()] [object]$ProcessIds, [bool]$Attempted = $true, [bool]$Succeeded = $true, [int]$ErrorCode = 0)
        $queryCopy = $jobIdentityTargetQuery | ConvertTo-Json -Depth 20 | ConvertFrom-Json
        $queryCopy.Attempted = $Attempted
        $queryCopy.Succeeded = $Succeeded
        $queryCopy.ErrorCode = $ErrorCode
        $queryCopy.ProcessIds = $ProcessIds
        return $queryCopy
    }
    $jobIdentityMalformedFreshQueryCases = @(
        (& $makeMalformedFreshJobQuery ([object[]]@($jobIdentityLaterPid, $jobIdentityLaterPid)))
        (& $makeMalformedFreshJobQuery ([object[]]@([double]$jobIdentityLaterPid + 0.5)))
        (& $makeMalformedFreshJobQuery ([object[]]@([string]$jobIdentityLaterPid)))
        (& $makeMalformedFreshJobQuery ([object[]]@([double]$jobIdentityLaterPid)))
        (& $makeMalformedFreshJobQuery $jobIdentityLaterPid)
        (& $makeMalformedFreshJobQuery ([object[]]@()) $false $true 0)
        (& $makeMalformedFreshJobQuery ([object[]]@()) $true $true 5)
        (& $makeMalformedFreshJobQuery ([object[]]@($jobIdentityLaterPid)) $true $true 0)
    )
    # The final case above is a valid query and is intentionally replaced with
    # a string Succeeded value to cover the boolean envelope too.
    $jobIdentityMalformedFreshQueryCases[7].Succeeded = 'true'
    $jobIdentityMalformedFreshQueryExpectedErrorCodes = [int[]]@(13, 13, 13, 13, 13, 13, 5, 13)
    $jobIdentityMalformedFreshQueryRejected = $true
    for ($malformedFreshIndex = 0; $malformedFreshIndex -lt $jobIdentityMalformedFreshQueryCases.Count; $malformedFreshIndex++) {
        $malformedFreshQuery = $jobIdentityMalformedFreshQueryCases[$malformedFreshIndex]
        $malformedFreshObservation = New-StartupCleanupObservation
        $malformedFreshQueryCalls = [pscustomobject]@{ Value = 0 }
        $malformedFreshCensusCalls = [pscustomobject]@{ Value = 0 }
        $malformedFreshInvokers = [pscustomobject][ordered]@{
            JobQuery = {
                param([IntPtr]$Handle)
                $malformedFreshQueryCalls.Value++
                if ($malformedFreshQueryCalls.Value -eq 1) { return $jobIdentityTargetQuery }
                return $malformedFreshQuery
            }
            ProcessCensus = {
                $malformedFreshCensusCalls.Value++
                return $jobIdentityTargetCensus
            }
            IdentityQuery = { param([int]$ProcessId, [int]$ParentProcessId) return $jobIdentityFailureProbe }
        }
        $malformedFreshThrew = $false
        try {
            [void](Get-JobProcessRecords ([IntPtr]1) @{} $malformedFreshObservation $malformedFreshObservation $malformedFreshInvokers)
        }
        catch { $malformedFreshThrew = $true }
        $malformedFreshTelemetry = $malformedFreshObservation.jobIdentityObservation
        if (-not ($malformedFreshThrew -and $malformedFreshQueryCalls.Value -eq 2 -and
                $malformedFreshCensusCalls.Value -eq 1 -and
                $malformedFreshObservation.query.queryCount -eq 2 -and
                $malformedFreshTelemetry.identityAttemptCount -eq 1 -and
                $malformedFreshTelemetry.identityFailureCount -eq 1 -and
                $malformedFreshTelemetry.recoveryAttemptCount -eq 1 -and
                $malformedFreshTelemetry.disappearedAfterSnapshotCount -eq 0 -and
                $malformedFreshTelemetry.stillPresentAfterFailureCount -eq 0 -and
                $malformedFreshTelemetry.failureType -eq 'enumeration-unavailable' -and
                $malformedFreshTelemetry.failureErrorCode -eq $jobIdentityMalformedFreshQueryExpectedErrorCodes[$malformedFreshIndex] -and
                $malformedFreshObservation.trackedSweepFailureType -eq 'none')) {
            $jobIdentityMalformedFreshQueryRejected = $false
        }
    }
    $jobIdentityMalformedFreshQueryCasesVerified = [bool]$jobIdentityMalformedFreshQueryRejected
    if (-not $jobIdentityMalformedFreshQueryCasesVerified) {
        throw 'Malformed fresh Job-query envelope self-test failed.'
    }

    # The Job query metadata is an untrusted native envelope too.  Keep the
    # initial query and census valid (with the target present), then inject one
    # malformed metadata field at a time into the fresh query.  These cases
    # exercise the production Get-JobProcessRecords validator rather than a
    # helper-only parser and must never qualify the identity disappearance.
    $makeMalformedFreshJobQueryMetadata = {
        param([string]$Kind)
        $queryCopy = & $makeJobIdentityQuery ([int[]]@($jobIdentityLaterPid))
        $makeAttempt = {
            param([int]$Number, [bool]$Succeeded, [int]$ErrorCode, [UInt64]$Capacity,
                [UInt64]$Required, [UInt64]$ReturnLength, [UInt32]$Assigned,
                [UInt32]$Listed, [bool]$Resized)
            return [pscustomobject][ordered]@{
                Attempted = $true; AttemptNumber = [int]$Number; Succeeded = $Succeeded
                ErrorCode = [int]$ErrorCode; CapacityBytes = [UInt64]$Capacity
                RequiredBytes = [UInt64]$Required; ReturnLengthBytes = [UInt64]$ReturnLength
                AssignedProcessCount = [UInt32]$Assigned; ListedProcessCount = [UInt32]$Listed
                Resized = $Resized
            }
        }
        $setSuccessfulFinal = {
            param([object]$FinalAttempt, [object[]]$Attempts)
            $queryCopy.Attempts = [object[]]$Attempts
            $queryCopy.AttemptCount = [int]$Attempts.Count
            $queryCopy.Succeeded = $true; $queryCopy.ErrorCode = [int]0
            $queryCopy.CapacityBytes = [UInt64]$FinalAttempt.CapacityBytes
            $queryCopy.RequiredBytes = [UInt64]$FinalAttempt.RequiredBytes
            $queryCopy.ReturnLengthBytes = [UInt64]$FinalAttempt.ReturnLengthBytes
            $queryCopy.AssignedProcessCount = [UInt32]$FinalAttempt.AssignedProcessCount
            $queryCopy.ListedProcessCount = [UInt32]$FinalAttempt.ListedProcessCount
            $queryCopy.Resized = [bool](@($Attempts | Where-Object { [bool]$_.Resized }).Count -gt 0)
            $queryCopy.ProcessIds = [int[]]@($jobIdentityLaterPid)
        }
        switch ($Kind) {
            'attempted-type' { $queryCopy.Attempted = 'true' }
            'attempt-count-type' { $queryCopy.AttemptCount = [double]1 }
            'attempt-count-range' { $queryCopy.AttemptCount = 9 }
            'capacity-type' { $queryCopy.CapacityBytes = [double]1024 }
            'capacity-range' { $queryCopy.CapacityBytes = [UInt64]1048577 }
            'required-type' { $queryCopy.RequiredBytes = '1024' }
            'return-length-type' { $queryCopy.ReturnLengthBytes = [double]1024 }
            'assigned-type' { $queryCopy.AssignedProcessCount = [double]1 }
            'listed-coherence' { $queryCopy.ListedProcessCount = [UInt32]2 }
            'resized-type' { $queryCopy.Resized = 'false' }
            'attempts-scalar' { $queryCopy.Attempts = $queryCopy.Attempts[0] }
            'attempts-length' { $queryCopy.AttemptCount = 2 }
            'attempt-number-type' { $queryCopy.Attempts[0].AttemptNumber = [double]1 }
            'attempt-capacity-type' { $queryCopy.Attempts[0].CapacityBytes = '1024' }
            'attempt-listed-coherence' { $queryCopy.Attempts[0].ListedProcessCount = [UInt32]2 }
            'attempt-resized-type' { $queryCopy.Attempts[0].Resized = 'false' }
            'last-status-mismatch' {
                $queryCopy.Attempts[0].Succeeded = $false
                $queryCopy.Attempts[0].ErrorCode = [int]24
            }
            'intermediate-complete-success' {
                $sizing = & $makeAttempt 1 $false 122 ([UInt64]0) ([UInt64]16) ([UInt64]16) ([UInt32]0) ([UInt32]0) $false
                $complete = & $makeAttempt 2 $true 0 ([UInt64]1024) ([UInt64]1024) ([UInt64]1024) ([UInt32]1) ([UInt32]1) $false
                $final = & $makeAttempt 3 $true 0 ([UInt64]2048) ([UInt64]2048) ([UInt64]2048) ([UInt32]1) ([UInt32]1) $false
                & $setSuccessfulFinal $final @($sizing, $complete, $final)
            }
            'intermediate-nonretryable-failure' {
                $sizing = & $makeAttempt 1 $false 122 ([UInt64]0) ([UInt64]16) ([UInt64]16) ([UInt32]0) ([UInt32]0) $false
                $failure = & $makeAttempt 2 $false 5 ([UInt64]1024) ([UInt64]1024) ([UInt64]1024) ([UInt32]0) ([UInt32]0) $false
                $final = & $makeAttempt 3 $true 0 ([UInt64]2048) ([UInt64]2048) ([UInt64]2048) ([UInt32]1) ([UInt32]1) $false
                & $setSuccessfulFinal $final @($sizing, $failure, $final)
            }
            'zero-sizing-return-mismatch' {
                $mismatch = & $makeAttempt 1 $false 122 ([UInt64]0) ([UInt64]16) ([UInt64]8) ([UInt32]0) ([UInt32]0) $false
                $queryCopy.Attempts = [object[]]@($mismatch)
                $queryCopy.AttemptCount = [int]1; $queryCopy.Succeeded = $false; $queryCopy.ErrorCode = [int]122
                $queryCopy.ProcessIds = [int[]]@(); $queryCopy.CapacityBytes = [UInt64]0
                $queryCopy.RequiredBytes = [UInt64]16; $queryCopy.ReturnLengthBytes = [UInt64]8
                $queryCopy.AssignedProcessCount = [UInt32]0; $queryCopy.ListedProcessCount = [UInt32]0
                $queryCopy.Resized = $false
            }
            'data-capacity-stagnant' {
                $sizing = & $makeAttempt 1 $false 122 ([UInt64]0) ([UInt64]16) ([UInt64]16) ([UInt32]0) ([UInt32]0) $false
                $partial = & $makeAttempt 2 $true 0 ([UInt64]1024) ([UInt64]1024) ([UInt64]1024) ([UInt32]2) ([UInt32]1) $true
                $final = & $makeAttempt 3 $true 0 ([UInt64]1024) ([UInt64]1024) ([UInt64]1024) ([UInt32]1) ([UInt32]1) $false
                & $setSuccessfulFinal $final @($sizing, $partial, $final)
            }
            default { throw "Unknown malformed Job-query metadata case: $Kind" }
        }
        return $queryCopy
    }
    $jobIdentityMalformedFreshQueryMetadataKinds = @(
        'attempted-type', 'attempt-count-type', 'attempt-count-range',
        'capacity-type', 'capacity-range', 'required-type', 'return-length-type',
        'assigned-type', 'listed-coherence', 'resized-type', 'attempts-scalar',
            'attempts-length', 'attempt-number-type', 'attempt-capacity-type',
        'attempt-listed-coherence', 'attempt-resized-type', 'last-status-mismatch',
        'intermediate-complete-success', 'intermediate-nonretryable-failure',
        'zero-sizing-return-mismatch', 'data-capacity-stagnant'
    )
    $jobIdentityMalformedFreshQueryMetadataCases = @(
        foreach ($metadataKind in $jobIdentityMalformedFreshQueryMetadataKinds) {
            & $makeMalformedFreshJobQueryMetadata $metadataKind
        }
    )
    $jobIdentityMalformedFreshQueryMetadataRejected = $true
    foreach ($malformedMetadataIndex in 0..($jobIdentityMalformedFreshQueryMetadataCases.Count - 1)) {
        $malformedMetadataQuery = $jobIdentityMalformedFreshQueryMetadataCases[$malformedMetadataIndex]
        $malformedMetadataObservation = New-StartupCleanupObservation
        $malformedMetadataQueryCalls = [pscustomobject]@{ Value = 0 }
        $malformedMetadataCensusCalls = [pscustomobject]@{ Value = 0 }
        $malformedMetadataInvokers = [pscustomobject][ordered]@{
            JobQuery = {
                param([IntPtr]$Handle)
                $malformedMetadataQueryCalls.Value++
                if ($malformedMetadataQueryCalls.Value -eq 1) { return $jobIdentityTargetQuery }
                return $malformedMetadataQuery
            }
            ProcessCensus = {
                $malformedMetadataCensusCalls.Value++
                return $jobIdentityTargetCensus
            }
            IdentityQuery = { param([int]$ProcessId, [int]$ParentProcessId) return $jobIdentityFailureProbe }
        }
        $malformedMetadataThrew = $false
        try {
            [void](Get-JobProcessRecords ([IntPtr]1) @{} $malformedMetadataObservation $malformedMetadataObservation $malformedMetadataInvokers)
        }
        catch { $malformedMetadataThrew = $true }
        $malformedMetadataTelemetry = $malformedMetadataObservation.jobIdentityObservation
        if (-not ($malformedMetadataThrew -and $malformedMetadataQueryCalls.Value -eq 2 -and
                $malformedMetadataCensusCalls.Value -eq 1 -and
                $malformedMetadataObservation.query.queryCount -eq 2 -and
                $malformedMetadataTelemetry.identityAttemptCount -eq 1 -and
                $malformedMetadataTelemetry.identityFailureCount -eq 1 -and
                $malformedMetadataTelemetry.recoveryAttemptCount -eq 1 -and
                $malformedMetadataTelemetry.disappearedAfterSnapshotCount -eq 0 -and
                $malformedMetadataTelemetry.stillPresentAfterFailureCount -eq 0 -and
                $malformedMetadataTelemetry.failureType -eq 'enumeration-unavailable' -and
                $malformedMetadataTelemetry.failureErrorCode -eq 13 -and
                $malformedMetadataObservation.trackedSweepFailureType -eq 'none')) {
            $jobIdentityMalformedFreshQueryMetadataRejected = $false
        }
    }
    $jobIdentityMalformedFreshQueryMetadataCasesVerified = [bool]$jobIdentityMalformedFreshQueryMetadataRejected
    if (-not $jobIdentityMalformedFreshQueryMetadataCasesVerified) {
        throw 'Malformed fresh Job-query metadata self-test failed.'
    }

    $jobIdentityProductionPathSelfTestVerified = $jobIdentityAcceptedVerified -and
        $jobIdentityStillPresentVerified -and $jobIdentityUnavailableVerified -and $jobIdentityMalformedVerified -and
        $jobIdentityInvocationExceptionVerified -and $jobIdentityFreshQueryExceptionVerified -and
        $jobIdentityConversionFailureVerified -and $jobIdentityContradictorySuccessVerified -and
        $jobIdentityMalformedFreshQueryCasesVerified -and $jobIdentityMalformedFreshQueryMetadataCasesVerified

    $descendantCreationParentSelfTest = [pscustomobject]@{ Id = 100; Creation = [long]1000 }
    $descendantCreationEqualSelfTest = [pscustomobject]@{ Id = 101; ParentId = 100; Creation = [long]1000 }
    $descendantCreationNewerSelfTest = [pscustomobject]@{ Id = 102; ParentId = 100; Creation = [long]1001 }
    $descendantCreationOlderSelfTest = [pscustomobject]@{ Id = 103; ParentId = 100; Creation = [long]999 }
    $descendantCreationMalformedSelfTest = [pscustomobject]@{ Id = 104; ParentId = 100; Creation = 'malformed' }
    $descendantCreationMismatchedParentSelfTest = [pscustomobject]@{ Id = 105; ParentId = 999; Creation = [long]1001 }
    if (-not (Test-StartupDescendantCreationOrder $descendantCreationParentSelfTest $descendantCreationEqualSelfTest) -or
        -not (Test-StartupDescendantCreationOrder $descendantCreationParentSelfTest $descendantCreationNewerSelfTest) -or
        (Test-StartupDescendantCreationOrder $descendantCreationParentSelfTest $descendantCreationOlderSelfTest) -or
        (Test-StartupDescendantCreationOrder $descendantCreationParentSelfTest $descendantCreationMalformedSelfTest) -or
        (Test-StartupDescendantCreationOrder $descendantCreationParentSelfTest $descendantCreationMismatchedParentSelfTest) -or
        (Test-StartupDescendantCreationOrder $null $descendantCreationNewerSelfTest)) {
        throw 'Descendant creation-order gate self-test failed.'
    }

    $descendantEntriesSelfTest = @(
        [pscustomobject][ordered]@{ ProcessId = 100; ParentProcessId = 0; ImageName = 'root.exe' }
        [pscustomobject][ordered]@{ ProcessId = 101; ParentProcessId = 100; ImageName = 'equal-child.exe' }
        [pscustomobject][ordered]@{ ProcessId = 102; ParentProcessId = 100; ImageName = 'new-child.exe' }
        [pscustomobject][ordered]@{ ProcessId = 103; ParentProcessId = 100; ImageName = 'stale-child.exe' }
        [pscustomobject][ordered]@{ ProcessId = 104; ParentProcessId = 103; ImageName = 'stale-grandchild.exe' }
    )
    $descendantIdentitySelfTest = @{
        100 = [pscustomobject][ordered]@{ ProcessId = 100; ParentProcessId = 0; CreationTime = [long]1000; ImagePath = 'C:\startup-descendant-selftest\root.exe' }
        101 = [pscustomobject][ordered]@{ ProcessId = 101; ParentProcessId = 100; CreationTime = [long]1000; ImagePath = 'C:\startup-descendant-selftest\equal-child.exe' }
        102 = [pscustomobject][ordered]@{ ProcessId = 102; ParentProcessId = 100; CreationTime = [long]1001; ImagePath = 'C:\startup-descendant-selftest\new-child.exe' }
        103 = [pscustomobject][ordered]@{ ProcessId = 103; ParentProcessId = 100; CreationTime = [long]999; ImagePath = 'C:\startup-descendant-selftest\stale-child.exe' }
        104 = [pscustomobject][ordered]@{ ProcessId = 104; ParentProcessId = 103; CreationTime = [long]1002; ImagePath = 'C:\startup-descendant-selftest\stale-grandchild.exe' }
    }
    $descendantProcessEntriesInvokerSelfTest = {
        return [pscustomobject][ordered]@{
            Attempted = $true; Complete = $true; Succeeded = $true; ErrorCode = 0
            AttemptCount = 1; RetryCount = 0; Retried = $false
            Entries = $descendantEntriesSelfTest
        }
    }
    $descendantIdentityInvokerSelfTest = {
        param($Entry)
        $entryId = [int](Get-StartupObservationProperty $Entry 'ProcessId')
        return [pscustomobject][ordered]@{
            Succeeded = $true; ErrorCode = 0; Identity = $descendantIdentitySelfTest[$entryId]
        }
    }
    $descendantSnapshotSelfTest = Get-ProcessSnapshot @{} ([int[]]@(100)) $null $null 'process-census-first' `
        $descendantIdentityInvokerSelfTest $descendantProcessEntriesInvokerSelfTest
    $descendantSnapshotIdsSelfTest = @($descendantSnapshotSelfTest.Keys | Sort-Object | ForEach-Object { [int]$_ })
    $descendantSnapshotSelfTestVerified = ($descendantSnapshotIdsSelfTest -join ',') -eq '100,101,102' -and
        -not $descendantSnapshotSelfTest.ContainsKey(103) -and -not $descendantSnapshotSelfTest.ContainsKey(104)

    $descendantOwnedSelfTest = @{
        100 = [pscustomobject][ordered]@{ Id = 100; ParentId = 0; Creation = [long]1000; ImagePath = 'C:\startup-descendant-selftest\root.exe' }
    }
    $descendantOwnedSnapshotSelfTest = @{
        100 = $descendantOwnedSelfTest[100]
        101 = [pscustomobject][ordered]@{ Id = 101; ParentId = 100; Creation = [long]1000; ImagePath = 'C:\startup-descendant-selftest\equal-child.exe' }
        102 = [pscustomobject][ordered]@{ Id = 102; ParentId = 100; Creation = [long]1001; ImagePath = 'C:\startup-descendant-selftest\new-child.exe' }
        103 = [pscustomobject][ordered]@{ Id = 103; ParentId = 100; Creation = [long]999; ImagePath = 'C:\startup-descendant-selftest\stale-child.exe' }
        104 = [pscustomobject][ordered]@{ Id = 104; ParentId = 103; Creation = [long]1002; ImagePath = 'C:\startup-descendant-selftest\stale-grandchild.exe' }
        105 = [pscustomobject][ordered]@{ Id = 105; ParentId = 100; Creation = 'malformed'; ImagePath = 'C:\startup-descendant-selftest\malformed-child.exe' }
    }
    Update-OwnedProcesses $descendantOwnedSelfTest $descendantOwnedSnapshotSelfTest
    $descendantOwnedIdsSelfTest = @($descendantOwnedSelfTest.Keys | Sort-Object | ForEach-Object { [int]$_ })
    $descendantOwnedSelfTestVerified = ($descendantOwnedIdsSelfTest -join ',') -eq '100,101,102' -and
        -not $descendantOwnedSelfTest.ContainsKey(103) -and -not $descendantOwnedSelfTest.ContainsKey(104) -and
        -not $descendantOwnedSelfTest.ContainsKey(105)
    $descendantCreationOrderSelfTestVerified = [bool]($descendantSnapshotSelfTestVerified -and $descendantOwnedSelfTestVerified)
    if (-not $descendantCreationOrderSelfTestVerified) {
        throw 'Descendant creation-order ownership self-test failed.'
    }

    $affinitySelfTestProbe = [pscustomobject][ordered]@{
        RequestedMask = [UInt64]1; ProcessMask = [UInt64]1; SystemMask = [UInt64]15
        Opened = $true; SetSucceeded = $true; ReadBackSucceeded = $true; Verified = $true
        DescendantsVerified = $false; ErrorCode = 0
    }
    $affinityMetadataSelfTest = Get-AffinityMetadata $affinitySelfTestProbe
    [void](Set-StartupAffinityLiveSetMetadata $affinityMetadataSelfTest 5 4 'tracked-sweep')
    $affinityHistoricalRecordsSelfTest = @{}
    for ($affinityId = 1; $affinityId -le 5; $affinityId++) {
        $affinityHistoricalRecordsSelfTest[$affinityId] = [pscustomobject]@{
            Id = $affinityId; ParentId = 0; Creation = [long](1000 + $affinityId)
            ImagePath = "C:\startup-affinity-selftest\process-$affinityId.exe"
        }
    }
    $affinityCurrentRecordsSelfTest = @(
        [pscustomobject]@{ Id = 1; ParentId = 0; Creation = [long]1001; ImagePath = 'C:\startup-affinity-selftest\process-1.exe' }
        [pscustomobject]@{ Id = 2; ParentId = 1; Creation = [long]1002; ImagePath = 'C:\startup-affinity-selftest\process-2.exe' }
        [pscustomobject]@{ Id = 3; ParentId = 1; Creation = [long]1003; ImagePath = 'C:\startup-affinity-selftest\process-3.exe' }
        [pscustomobject]@{ Id = 4; ParentId = 2; Creation = [long]1004; ImagePath = 'C:\startup-affinity-selftest\process-4.exe' }
    )
    $affinityReadBackIdsSelfTest = New-Object Collections.Generic.List[int]
    $affinityReadBackSelfTest = {
        param($Record, $Mask)
        if ([UInt64]$Mask -ne [UInt64]1) { throw 'Synthetic affinity read-back mask mismatch.' }
        [void]$affinityReadBackIdsSelfTest.Add([int]$Record.Id)
    }
    $affinityCurrentPlanSelfTest = @(Get-StartupAffinityVerificationPlan $affinityHistoricalRecordsSelfTest $affinityCurrentRecordsSelfTest)
    foreach ($record in $affinityCurrentPlanSelfTest) {
        [void](& $affinityReadBackSelfTest $record ([UInt64]1))
    }
    $affinityPlanIdsSelfTest = @($affinityReadBackIdsSelfTest.ToArray())
    $affinityCurrentSetSelfTestVerified = $affinityCurrentPlanSelfTest.Count -eq 4 -and
        $affinityReadBackIdsSelfTest.Count -eq 4 -and ($affinityPlanIdsSelfTest -join ',') -eq '1,2,3,4' -and
        -not ($affinityPlanIdsSelfTest -contains 5)
    if (-not $affinityCurrentSetSelfTestVerified) { throw 'Current-live affinity verification plan self-test failed.' }

    $affinityEmptyRejected = $false
    try { [void](Get-StartupAffinityVerificationPlan $affinityHistoricalRecordsSelfTest @()) }
    catch { $affinityEmptyRejected = $true }
    $affinityNullRejected = $false
    try { [void](Get-StartupAffinityVerificationPlan $affinityHistoricalRecordsSelfTest $null) }
    catch { $affinityNullRejected = $true }
    $affinityUnknownRejected = $false
    try {
        [void](Get-StartupAffinityVerificationPlan $affinityHistoricalRecordsSelfTest @(
            $affinityCurrentRecordsSelfTest
            [pscustomobject]@{ Id = 99; ParentId = 0; Creation = [long]1099; ImagePath = 'C:\startup-affinity-selftest\unknown.exe' }
        ))
    }
    catch { $affinityUnknownRejected = $true }
    $affinityDuplicateRejected = $false
    try {
        [void](Get-StartupAffinityVerificationPlan $affinityHistoricalRecordsSelfTest @(
            $affinityCurrentRecordsSelfTest[0]
            $affinityCurrentRecordsSelfTest[1]
            $affinityCurrentRecordsSelfTest[1]
            $affinityCurrentRecordsSelfTest[2]
            $affinityCurrentRecordsSelfTest[3]
        ))
    }
    catch { $affinityDuplicateRejected = $true }
    $affinityCreationMismatchRejected = $false
    try {
        [void](Get-StartupAffinityVerificationPlan $affinityHistoricalRecordsSelfTest @(
            $affinityCurrentRecordsSelfTest[0]
            $affinityCurrentRecordsSelfTest[1]
            $affinityCurrentRecordsSelfTest[2]
            [pscustomobject]@{ Id = 4; ParentId = 2; Creation = [long]9999; ImagePath = 'C:\startup-affinity-selftest\process-4.exe' }
        ))
    }
    catch { $affinityCreationMismatchRejected = $true }
    $affinityPathMismatchRejected = $false
    try {
        [void](Get-StartupAffinityVerificationPlan $affinityHistoricalRecordsSelfTest @(
            $affinityCurrentRecordsSelfTest[0]
            $affinityCurrentRecordsSelfTest[1]
            $affinityCurrentRecordsSelfTest[2]
            [pscustomobject]@{ Id = 4; ParentId = 2; Creation = [long]1004; ImagePath = 'C:\startup-affinity-selftest\replacement.exe' }
        ))
    }
    catch { $affinityPathMismatchRejected = $true }
    $affinityInvalidCurrentSetSelfTestVerified = $affinityEmptyRejected -and $affinityNullRejected -and
        $affinityUnknownRejected -and $affinityDuplicateRejected -and $affinityCreationMismatchRejected -and
        $affinityPathMismatchRejected
    if (-not $affinityInvalidCurrentSetSelfTestVerified) { throw 'Invalid current-live affinity set self-test failed.' }

    $affinityMetadataSelfTest.descendantsVerified = $true
    $affinityHistoricalCountsVerified = $affinityMetadataSelfTest.historicalOwnedCount -eq 5 -and
        $affinityMetadataSelfTest.currentLiveCount -eq 4 -and
        $affinityMetadataSelfTest.expiredHistoricalCount -eq 1 -and
        $affinityMetadataSelfTest.liveSetSource -eq 'tracked-sweep' -and
        $affinityMetadataSelfTest.verified -and $affinityMetadataSelfTest.descendantsVerified -and
        $affinityCurrentSetSelfTestVerified -and $affinityInvalidCurrentSetSelfTestVerified
    if (-not $affinityHistoricalCountsVerified) { throw 'Historical/current affinity count self-test failed.' }
    $syntheticFailedQuery = [StartupProbeJobResult][pscustomobject][ordered]@{
        Attempted = $true
        AttemptCount = 1
        CapacityBytes = [UInt64]0
        RequiredBytes = [UInt64]8
        ReturnLengthBytes = [UInt64]8
        AssignedProcessCount = [UInt64]2
        ListedProcessCount = [UInt64]0
        Resized = $false
        Succeeded = $false
        ErrorCode = 234
        Attempts = @([pscustomobject][ordered]@{
                Attempted = $true; AttemptNumber = 1; Succeeded = $false; ErrorCode = 234
                CapacityBytes = [UInt64]0; RequiredBytes = [UInt64]8; ReturnLengthBytes = [UInt64]8
                AssignedProcessCount = [UInt64]2; ListedProcessCount = [UInt64]0; Resized = $false
            })
        ProcessIds = @([int]1234)
    }
    $syntheticFailedObservation = Convert-StartupJobQueryObservation $syntheticFailedQuery
    $syntheticFailedJson = $syntheticFailedObservation | ConvertTo-Json -Depth 8 -Compress
    $jobQueryError234Retained = ($syntheticFailedObservation.attempted -and $syntheticFailedObservation.queryCount -eq 1 -and
        -not $syntheticFailedObservation.succeeded -and
        $syntheticFailedObservation.errorCode -eq 234 -and $syntheticFailedObservation.attemptCount -eq 1 -and
        $syntheticFailedObservation.attempts.Count -eq 1 -and $syntheticFailedObservation.attempts[0].errorCode -eq 234 -and
        $syntheticFailedJson -notmatch '(?i)processids|imagepath|commandline|payload')
    if (-not $jobQueryError234Retained) {
        throw 'Failed job query observation self-test failed.'
    }
    $malformedQuery = [pscustomobject][ordered]@{
        Attempted = $true
        Succeeded = $false
        ErrorCode = [pscustomobject]@{}
        AttemptCount = [pscustomobject]@{}
        CapacityBytes = [pscustomobject]@{}
        RequiredBytes = [pscustomobject]@{}
        ReturnLengthBytes = [pscustomobject]@{}
        AssignedProcessCount = [pscustomobject]@{}
        ListedProcessCount = [pscustomobject]@{}
        Resized = [pscustomobject]@{}
        Attempts = [pscustomobject][ordered]@{ payload = 'secret'; ProcessIds = @([int]1234) }
        ProcessIds = @([int]1234)
    }
    $malformedObservation = Convert-StartupJobQueryObservation $malformedQuery
    $malformedObservationJson = $malformedObservation | ConvertTo-Json -Depth 8 -Compress
    $malformedObservationSelfTestVerified = $malformedObservation.attempts.Count -le 8 -and
        $malformedObservationJson -notmatch '(?i)processids|imagepath|commandline|payload'
    if (-not $malformedObservationSelfTestVerified) { throw 'Malformed job query observation self-test failed.' }
    $jobQueryObservationSelfTestVerified = $jobQueryObservationSelfTestVerified -and $malformedObservationSelfTestVerified
    $failedCleanupObservation = New-StartupCleanupObservation
    $failedCleanupObservation.jobPresent = $true
    $failedCleanupObservation.jobQueryAttempted = $true
    $failedCleanupObservation.jobQuerySucceeded = $false
    $failedCleanupObservation.jobCloseSucceeded = $true
    $failedCleanupObservation.finalPathSweepVerified = $true
    $failedCleanupObservation.survivorCount = 0
    $failedProcessCleanupVerified = $failedCleanupObservation.survivorCount -eq 0 -and
        $failedCleanupObservation.jobQuerySucceeded -and $failedCleanupObservation.jobCloseSucceeded -and
        $failedCleanupObservation.finalPathSweepVerified
    $failedQueryCleanupRemainsUnverified = -not $failedProcessCleanupVerified
    if (-not $failedQueryCleanupRemainsUnverified) { throw 'Failed job query with no survivors was treated as verified cleanup.' }
    $cleanupErrorCountSelfTestObservation = New-StartupCleanupObservation
    $cleanupErrorCountSelfTestObservation.cleanupErrorCount = 2
    $cleanupErrorCountSelfTestTotal = Get-StartupCleanupErrorCount $cleanupErrorCountSelfTestObservation 2 $true
    $cleanupErrorCountOverflowSelfTestObservation = New-StartupCleanupObservation
    $cleanupErrorCountOverflowSelfTestObservation.cleanupErrorCount = [int]::MaxValue
    $cleanupErrorCountOverflowSelfTestTotal = Get-StartupCleanupErrorCount $cleanupErrorCountOverflowSelfTestObservation ([Int64][int]::MaxValue)
    $cleanupErrorCountSelfTestVerified = $cleanupErrorCountSelfTestTotal -eq 3 -and
        $cleanupErrorCountOverflowSelfTestTotal -eq [int]::MaxValue
    if (-not $cleanupErrorCountSelfTestVerified) { throw 'Cleanup error count aggregation self-test failed.' }
    $gracefulTerminalObservation = New-StartupCleanupObservation
    $gracefulTerminalObservation.jobQuerySucceeded = $true
    $gracefulTerminalObservation.jobCloseSucceeded = $true
    $gracefulTerminalObservation.trackedSweepVerified = $true
    $gracefulTerminalObservation.finalPathSweepVerified = $true
    $gracefulTerminalProof = New-StartupContainmentProof
    $gracefulTerminalProof.mode = 'graceful-job-empty'
    $gracefulTerminalProof.terminalState = 'verified-graceful-job-empty'
    $gracefulTerminalProof.terminalJobQuery = (Convert-StartupTerminalJobQuery $emptyJob).observation
    $gracefulTerminalProof.terminalJobMemberCount = 0
    $gracefulTerminalProof.jobEmptyProven = $true
    $gracefulTerminalObservation.containmentProof = $gracefulTerminalProof
    $gracefulTerminalCleanup = [pscustomobject][ordered]@{
        survivors = @()
        jobQuerySucceeded = $true
        jobCloseSucceeded = $true
        finalPathSweepVerified = $true
        containmentProof = $gracefulTerminalProof
        cleanupObservation = $gracefulTerminalObservation
    }
    $gracefulTerminalAccepted = Test-StartupProcessCleanupCompletion $gracefulTerminalCleanup ([IntPtr]::Zero) 0
    $legacyZeroEvidence = New-StartupContainmentProof
    $gracefulTerminalCleanup.containmentProof = $legacyZeroEvidence
    $externalZeroWithoutJobProofRejected = -not (Test-StartupProcessCleanupCompletion $gracefulTerminalCleanup ([IntPtr]::Zero) 0)
    $gracefulTerminalCleanup.containmentProof = $gracefulTerminalProof
    $gracefulTerminalCleanup.jobQuerySucceeded = $false
    $gracefulTerminalBadQueryRejected = -not (Test-StartupProcessCleanupCompletion $gracefulTerminalCleanup ([IntPtr]::Zero) 0)
    $gracefulTerminalCleanup.jobQuerySucceeded = $true
    $gracefulTerminalCleanup.jobCloseSucceeded = $false
    $gracefulTerminalBadCloseRejected = -not (Test-StartupProcessCleanupCompletion $gracefulTerminalCleanup ([IntPtr]::Zero) 0)
    $gracefulTerminalCleanup.jobCloseSucceeded = $true
    $gracefulTerminalObservation.trackedSweepVerified = $false
    $gracefulTerminalBadTrackedRejected = -not (Test-StartupProcessCleanupCompletion $gracefulTerminalCleanup ([IntPtr]::Zero) 0)
    $gracefulTerminalObservation.trackedSweepVerified = $true
    $gracefulTerminalCleanup.finalPathSweepVerified = $false
    $gracefulTerminalBadPathRejected = -not (Test-StartupProcessCleanupCompletion $gracefulTerminalCleanup ([IntPtr]::Zero) 0)
    $gracefulTerminalCleanup.finalPathSweepVerified = $true
    $gracefulTerminalCompletionSelfTestVerified = $gracefulTerminalAccepted -and
        $externalZeroWithoutJobProofRejected -and $gracefulTerminalBadQueryRejected -and
        $gracefulTerminalBadCloseRejected -and $gracefulTerminalBadTrackedRejected -and
        $gracefulTerminalBadPathRejected
    if (-not $gracefulTerminalCompletionSelfTestVerified) {
        throw 'Graceful fallback terminal-gate self-test failed.'
    }
    $jobQueryRetryContractSelfTestVerified = [NativeStartupProbe]::RunJobQueryContractSelfTest()
    if (-not $jobQueryRetryContractSelfTestVerified) {
        throw 'Bounded job-query retry contract self-test failed.'
    }
    $jobQueryRetryBehaviorCorrected = [bool]$jobQueryRetryContractSelfTestVerified
    $processEnumerationRetryContractSelfTestVerified = [NativeStartupProbe]::RunProcessEnumerationContractSelfTest()
    if (-not $processEnumerationRetryContractSelfTestVerified) {
        throw 'Bounded process-enumeration retry contract self-test failed.'
    }
    $processEnumerationRetryBehaviorCorrected = [bool]$processEnumerationRetryContractSelfTestVerified
    [void](Get-ProcessSnapshot @{} ([int[]]@($PID)))
    $snapshotWatch = [Diagnostics.Stopwatch]::StartNew()
    $snapshot = Get-ProcessSnapshot @{} ([int[]]@($PID))
    $snapshotWatch.Stop()
    $realProcessEnumerationSelfTestVerified = $snapshot.ContainsKey($PID)
    if (-not $realProcessEnumerationSelfTestVerified) { throw 'Native process snapshot self-test failed.' }

    # Exercise the diagnostic checkpoint boundary without starting a process.
    # A successful Job membership observation followed by a census/window
    # failure must remain unavailable, with the failure substage and numeric
    # error retained instead of collapsing to a generic process-tree label.
    $diagnosticFakeExit = [pscustomobject]@{
        Succeeded = $true; Active = $true; ExitCode = [UInt32]259; ErrorCode = 0
    }
    $diagnosticFakeExitFailure = [pscustomobject]@{
        Succeeded = $false; Active = $false; ExitCode = [UInt32]0; ErrorCode = 5
    }
    $diagnosticFakeQuery = New-StartupJobQueryObservation
    $diagnosticFakeQuery.attempted = $true
    $diagnosticFakeQuery.succeeded = $true
    $diagnosticFakeQuery.errorCode = 0
    $diagnosticFakeQuery.queryCount = 1
    $diagnosticFakeQuery.attemptCount = 1
    $diagnosticFakeQuery.attempts = @([ordered]@{
            attempted = $true; attemptNumber = 1; succeeded = $true; errorCode = 0
            capacityBytes = [UInt64]0; requiredBytes = [UInt64]0; returnLengthBytes = [UInt64]0
            assignedProcessCount = [UInt64]0; listedProcessCount = [UInt64]0; resized = $false
        })
    $diagnosticFakeJob = [pscustomobject]@{
        verified = $true; members = @{}; queryObservation = $diagnosticFakeQuery
    }
    $diagnosticBaseExitInvoker = { param($Handle) $diagnosticFakeExit }.GetNewClosure()
    $diagnosticBaseJobInvoker = { param($Handle) $diagnosticFakeJob }.GetNewClosure()
    $diagnosticCheckpointInvoker = {
        param(
            [Parameter(Mandatory = $true)] [string]$ExpectedSubstage,
            [AllowNull()] [scriptblock]$ExitInvoker = $null,
            [AllowNull()] [scriptblock]$JobInvoker = $null,
            [AllowNull()] [scriptblock]$FirstProcessInvoker = $null,
            [AllowNull()] [scriptblock]$SecondProcessInvoker = $null,
            [AllowNull()] [scriptblock]$WindowInvoker = $null,
            [AllowNull()] [scriptblock]$ProjectionInvoker = $null,
            [int]$ExpectedErrorCode = 13
        )
        $testState = New-StartupDiagnosticState 1234
        $testInvokers = [ordered]@{
            ExitProbe = if ($null -eq $ExitInvoker) { $diagnosticBaseExitInvoker } else { $ExitInvoker }
            JobMembers = if ($null -eq $JobInvoker) { $diagnosticBaseJobInvoker } else { $JobInvoker }
        }
        if ($null -ne $FirstProcessInvoker) { $testInvokers.ProcessSnapshotFirst = $FirstProcessInvoker }
        if ($null -ne $SecondProcessInvoker) { $testInvokers.ProcessSnapshotSecond = $SecondProcessInvoker }
        if ($null -ne $WindowInvoker) { $testInvokers.WindowCount = $WindowInvoker }
        if ($null -ne $ProjectionInvoker) { $testInvokers.ProjectionFinalize = $ProjectionInvoker }
        [void](Add-StartupDiagnosticCheckpoint -State $testState -Checkpoint '2s' -Owned @{} -Job ([IntPtr]1) `
                -ElapsedMs 2000 -Invokers $testInvokers)
        $testSnapshot = @($testState.processTreeSnapshots | Where-Object { $_.checkpoint -eq '2s' })[0]
        if ($null -eq $testSnapshot -or $testSnapshot.status -ne 'unavailable' -or
            $testSnapshot.observed -or $testSnapshot.failureSubstage -ne $ExpectedSubstage -or
            $testSnapshot.failureErrorCode -ne $ExpectedErrorCode) {
            throw "Diagnostic substage self-test failed for $ExpectedSubstage."
        }
        return $testSnapshot
    }.GetNewClosure()
    $diagnosticRootFailureSnapshot = & $diagnosticCheckpointInvoker 'root-exit' `
        ({ param($Handle) $diagnosticFakeExitFailure }.GetNewClosure()) $null $null $null $null $null 5
    $diagnosticJobFailureSnapshot = & $diagnosticCheckpointInvoker 'job-membership' $null `
        ({ throw 'synthetic Job membership failure' }.GetNewClosure())
    $diagnosticCensusFirstFailureSnapshot = & $diagnosticCheckpointInvoker 'process-census-first' $null $null `
        ({ param($Owned, $Phase) throw 'synthetic first process census failure' }.GetNewClosure())
    $diagnosticIdentityFirstFailureSnapshot = & $diagnosticCheckpointInvoker 'process-identity-first' $null $null `
        ({ param($Owned, $Phase) $Phase.substage = 'process-identity-first'; throw 'synthetic first identity failure' }.GetNewClosure())
    $diagnosticCensusSecondFailureSnapshot = & $diagnosticCheckpointInvoker 'process-census-second' $null $null `
        ({ param($Owned, $Phase) return @{} }.GetNewClosure()) `
        ({ param($Owned, $Phase) throw 'synthetic second process census failure' }.GetNewClosure())
    $diagnosticIdentitySecondFailureSnapshot = & $diagnosticCheckpointInvoker 'process-identity-second' $null $null `
        ({ param($Owned, $Phase) return @{} }.GetNewClosure()) `
        ({ param($Owned, $Phase) $Phase.substage = 'process-identity-second'; throw 'synthetic second identity failure' }.GetNewClosure())
    $diagnosticWindowFailureSnapshot = & $diagnosticCheckpointInvoker 'window-enumeration' $null $null `
        ({ param($Owned, $Phase) return @{} }.GetNewClosure()) `
        ({ param($Owned, $Phase) return @{} }.GetNewClosure()) `
        ({ param($ProcessIds) throw 'synthetic window enumeration failure' }.GetNewClosure())
    $diagnosticProjectionFailureSnapshot = & $diagnosticCheckpointInvoker 'projection-finalize' $null $null `
        ({ param($Owned, $Phase) return @{} }.GetNewClosure()) `
        ({ param($Owned, $Phase) return @{} }.GetNewClosure()) `
        ({ param($ProcessIds) [pscustomobject]@{ count = 0; capped = $false; editorWindowCount = 0; dialogWindowCount = 0; otherWindowCount = 0 } }.GetNewClosure()) `
        ({ param($Records, $WindowInfo) throw 'synthetic projection failure' }.GetNewClosure())
    $diagnosticCheckpointSubstageSelfTestVerified = [bool](
        $diagnosticRootFailureSnapshot.failureErrorCode -eq 5 -and
        $diagnosticJobFailureSnapshot.failureErrorCode -eq 13 -and
        $diagnosticCensusFirstFailureSnapshot.jobQueryObservation.succeeded -and
        $diagnosticCensusFirstFailureSnapshot.jobMembershipVerified -and
        $diagnosticCensusSecondFailureSnapshot.jobQueryObservation.succeeded -and
        $diagnosticWindowFailureSnapshot.jobQueryObservation.succeeded -and
        $diagnosticProjectionFailureSnapshot.jobQueryObservation.succeeded)
    if (-not $diagnosticCheckpointSubstageSelfTestVerified) {
        throw 'Diagnostic checkpoint substage self-test did not preserve successful Job evidence.'
    }
    $diagnosticProcessEntry = [pscustomobject][ordered]@{
        ProcessId = [int]1234; ParentProcessId = [int]0; ImageName = 'sakura.exe'
    }
    $diagnosticProcessEntriesProbe = {
        param([object[]]$Entries)
        [pscustomobject][ordered]@{
            Attempted = $true; Complete = $true; Succeeded = $true; ErrorCode = [int]0
            AttemptCount = [int]1; RetryCount = [int]0; Retried = $false; Entries = $Entries
        }
    }.GetNewClosure()
    $diagnosticIdentityFailure = [pscustomobject][ordered]@{
        Succeeded = $false; Identity = $null; ErrorCode = [int]5
    }
    $diagnosticStillPresentPhase = [ordered]@{ substage = 'process-identity-first'; errorCode = $null }
    $diagnosticStillPresentCaught = $false
    try {
        Set-StartupDiagnosticPhase $diagnosticStillPresentPhase 'process-identity-first'
        [void](Resolve-StartupProcessIdentity $diagnosticProcessEntry $diagnosticIdentityFailure `
            (New-StartupDiagnosticProcessObservation) $diagnosticStillPresentPhase `
            'process-census-first' 'process-identity-first' `
            ({ & $diagnosticProcessEntriesProbe ([object[]]@($diagnosticProcessEntry)) }.GetNewClosure()))
    }
    catch { $diagnosticStillPresentCaught = $true }
    $diagnosticStillPresentSecondPhase = [ordered]@{ substage = 'process-identity-second'; errorCode = $null }
    $diagnosticStillPresentSecondCaught = $false
    try {
        Set-StartupDiagnosticPhase $diagnosticStillPresentSecondPhase 'process-identity-second'
        [void](Resolve-StartupProcessIdentity $diagnosticProcessEntry $diagnosticIdentityFailure `
            (New-StartupDiagnosticProcessObservation) $diagnosticStillPresentSecondPhase `
            'process-census-second' 'process-identity-second' `
            ({ & $diagnosticProcessEntriesProbe ([object[]]@($diagnosticProcessEntry)) }.GetNewClosure()))
    }
    catch { $diagnosticStillPresentSecondCaught = $true }
    $diagnosticProductionIdentityStillPresentSelfTestVerified = [bool](
        $diagnosticStillPresentCaught -and
        [StringComparer]::Ordinal.Equals([string]$diagnosticStillPresentPhase.substage, 'process-identity-first') -and
        $diagnosticStillPresentPhase.errorCode -eq 5 -and
        $diagnosticStillPresentSecondCaught -and
        [StringComparer]::Ordinal.Equals([string]$diagnosticStillPresentSecondPhase.substage, 'process-identity-second') -and
        $diagnosticStillPresentSecondPhase.errorCode -eq 5)
    if (-not $diagnosticProductionIdentityStillPresentSelfTestVerified) {
        throw 'Production process identity still-present self-test failed.'
    }
    $diagnosticIdentityOwned = @{
        1234 = [pscustomobject][ordered]@{
            Id = [int]1234; ParentId = [int]0; Creation = [Int64]1; ImagePath = 'C:\sakura.exe'
        }
    }
    $diagnosticValidIdentity = [pscustomobject][ordered]@{
        ProcessId = [int]1234; ParentProcessId = [int]0; CreationTime = [Int64]1; ImagePath = 'C:\sakura.exe'
    }
    $diagnosticMalformedIdentityCases = @(
        [ordered]@{
            name = 'string-success'
            probe = [pscustomobject][ordered]@{ Succeeded = 'true'; ErrorCode = [int]0; Identity = $diagnosticValidIdentity }
            expectedErrorCode = [int]13
        }
        [ordered]@{
            name = 'success-error'
            probe = [pscustomobject][ordered]@{ Succeeded = $true; ErrorCode = [int]5; Identity = $diagnosticValidIdentity }
            expectedErrorCode = [int]5
        }
        [ordered]@{
            name = 'pid-mismatch'
            probe = [pscustomobject][ordered]@{
                Succeeded = $true; ErrorCode = [int]0
                Identity = [pscustomobject][ordered]@{
                    ProcessId = [int]4321; ParentProcessId = [int]0; CreationTime = [Int64]1; ImagePath = 'C:\sakura.exe'
                }
            }
            expectedErrorCode = [int]13
        }
        [ordered]@{
            name = 'ppid-mismatch'
            probe = [pscustomobject][ordered]@{
                Succeeded = $true; ErrorCode = [int]0
                Identity = [pscustomobject][ordered]@{
                    ProcessId = [int]1234; ParentProcessId = [int]4321; CreationTime = [Int64]1; ImagePath = 'C:\sakura.exe'
                }
            }
            expectedErrorCode = [int]13
        }
    )
    $diagnosticMalformedIdentitySelfTestVerified = $true
    foreach ($identityCase in @($diagnosticMalformedIdentityCases)) {
        $identityCensusCalls = [pscustomobject]@{ value = 0 }
        $identityQueryCalls = [pscustomobject]@{ value = 0 }
        $identityEntriesInvoker = {
            $identityCensusCalls.value++
            & $diagnosticProcessEntriesProbe ([object[]]@($diagnosticProcessEntry))
        }.GetNewClosure()
        $identityInvoker = {
            param($Entry)
            $identityQueryCalls.value++
            return $identityCase.probe
        }.GetNewClosure()
        $identityPhase = [ordered]@{ substage = 'process-census-first'; errorCode = $null }
        $identityCaught = $false
        try {
            [void](Get-ProcessSnapshot $diagnosticIdentityOwned @() (New-StartupDiagnosticProcessObservation) `
                $identityPhase 'process-census-first' $identityInvoker $identityEntriesInvoker)
        }
        catch { $identityCaught = $true }
        if (-not ($identityCaught -and $identityCensusCalls.value -eq 1 -and
                $identityQueryCalls.value -eq 1 -and
                [StringComparer]::Ordinal.Equals([string]$identityPhase.substage, 'process-identity-first') -and
                $identityPhase.errorCode -eq $identityCase.expectedErrorCode)) {
            $diagnosticMalformedIdentitySelfTestVerified = $false
        }
    }
    if (-not $diagnosticMalformedIdentitySelfTestVerified) {
        throw 'Malformed process identity success envelope was accepted or retried.'
    }
    # Get-VerifiedProcessEntries invokes an injected census without arguments.
    # Keep the counter in a mutable object so the GetNewClosure child scope
    # updates the value observed by this self-test.
    $diagnosticDisappearanceEntriesCallCounter = [pscustomobject]@{ value = 0 }
    $diagnosticUnrelatedProcessEntry = [pscustomobject][ordered]@{
        ProcessId = [int]5678; ParentProcessId = [int]0; ImageName = 'other.exe'
    }
    $diagnosticDisappearanceEntriesInvoker = {
        $diagnosticDisappearanceEntriesCallCounter.value++
        $entries = if ($diagnosticDisappearanceEntriesCallCounter.value -eq 1) {
            [object[]]@($diagnosticProcessEntry)
        }
        else { [object[]]@($diagnosticUnrelatedProcessEntry) }
        & $diagnosticProcessEntriesProbe $entries
    }.GetNewClosure()
    $diagnosticDisappearanceIdentityInvoker = ({ param($Entry) $diagnosticIdentityFailure }.GetNewClosure())
    $diagnosticDisappearanceWindowInvoker = ({ param($ProcessIds) throw 'synthetic window enumeration failure after disappearance' }.GetNewClosure())
    $diagnosticDisappearanceState = New-StartupDiagnosticState 1234
    $diagnosticDisappearanceOwned = @{
        1234 = [pscustomobject][ordered]@{
            Id = [int]1234; ParentId = [int]0; Creation = [Int64]1; ImagePath = 'C:\sakura.exe'
        }
    }
    $diagnosticDisappearanceInvokers = [ordered]@{
        ExitProbe = $diagnosticBaseExitInvoker
        JobMembers = $diagnosticBaseJobInvoker
        ProcessIdentity = $diagnosticDisappearanceIdentityInvoker
        ProcessEntries = $diagnosticDisappearanceEntriesInvoker
        WindowCount = $diagnosticDisappearanceWindowInvoker
    }
    [void](Add-StartupDiagnosticCheckpoint -State $diagnosticDisappearanceState -Checkpoint '2s' `
        -Owned $diagnosticDisappearanceOwned -Job ([IntPtr]1) -ElapsedMs 2000 `
        -Invokers $diagnosticDisappearanceInvokers)
    $diagnosticDisappearanceSnapshot = @($diagnosticDisappearanceState.processTreeSnapshots |
        Where-Object { $_.checkpoint -eq '2s' })[0]
    $diagnosticProductionDisappearanceWindowSelfTestVerified = [bool](
        $diagnosticDisappearanceEntriesCallCounter.value -ge 3 -and
        $diagnosticDisappearanceSnapshot.status -eq 'unavailable' -and
        [StringComparer]::Ordinal.Equals([string]$diagnosticDisappearanceSnapshot.failureSubstage, 'window-enumeration') -and
        $diagnosticDisappearanceSnapshot.failureErrorCode -eq 13 -and
        $diagnosticDisappearanceSnapshot.jobQueryObservation.succeeded)
    if (-not $diagnosticProductionDisappearanceWindowSelfTestVerified) {
        throw 'Benign identity disappearance leaked into the window diagnostic phase.'
    }
    if ((Get-Sha256 $PSCommandPath).Length -ne 64) { throw 'SHA-256 self-test failed.' }
    if ([NativeStartupProbe]::GetVerticalScrollMaximum([IntPtr]::Zero) -ne -1) { throw 'Scroll-range self-test failed.' }
    $selfTestTraceRoot = [IO.Path]::GetFullPath((Join-Path $HOME 'tmp'))
    [IO.Directory]::CreateDirectory($selfTestTraceRoot) | Out-Null
    $traceBoundsSelfTest = Get-StartupTraceBounds
    if ($traceBoundsSelfTest.maxFiles -ne 8 -or $traceBoundsSelfTest.maxBytes -ne 1048576 -or
        $traceBoundsSelfTest.maxLines -ne 4096 -or $traceBoundsSelfTest.maxValidRecords -ne 4096 -or
        $traceBoundsSelfTest.maxLineLength -ne 65536) {
        throw 'Startup trace bounds contract self-test failed.'
    }
    $selfTestEmptyTraceDirectory = Join-Path $selfTestTraceRoot ('sakuracode-startup-trace-empty-selftest-' + [Guid]::NewGuid().ToString('N'))
    $selfTestOverLimitTraceDirectory = Join-Path $selfTestTraceRoot ('sakuracode-startup-trace-overlimit-selftest-' + [Guid]::NewGuid().ToString('N'))
    try {
        [IO.Directory]::CreateDirectory($selfTestEmptyTraceDirectory) | Out-Null
        $emptyTraceSummary = Get-StartupTraceSummary $selfTestEmptyTraceDirectory 1000 1000
        if ($emptyTraceSummary.collected -or $emptyTraceSummary.fileCount -ne 0 -or
            $emptyTraceSummary.parseErrors.Count -ne 1 -or $emptyTraceSummary.parseErrors[0].error -ne 'StartupTraceEmpty') {
            throw 'Empty startup trace summary self-test failed.'
        }
        [IO.Directory]::CreateDirectory($selfTestOverLimitTraceDirectory) | Out-Null
        $overLimitTraceFile = Join-Path $selfTestOverLimitTraceDirectory 'startup-trace-overlimit.jsonl'
        $overLimitStream = New-Object IO.FileStream($overLimitTraceFile, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
        try { $overLimitStream.SetLength([Int64]($traceBoundsSelfTest.maxBytes + 1)) }
        finally { $overLimitStream.Dispose() }
        $overLimitTraceSummary = Get-StartupTraceSummary $selfTestOverLimitTraceDirectory 1000 1000
        if ($overLimitTraceSummary.collected -or $overLimitTraceSummary.totalBytes -ne 0 -or
            $overLimitTraceSummary.parseErrors.Count -ne 1 -or $overLimitTraceSummary.parseErrors[0].error -ne 'StartupTraceBoundsExceeded:bytes') {
            throw 'Over-limit startup trace summary self-test failed.'
        }
    }
    finally {
        if (Test-Path -LiteralPath $selfTestEmptyTraceDirectory) { [IO.Directory]::Delete($selfTestEmptyTraceDirectory, $true) }
        if (Test-Path -LiteralPath $selfTestOverLimitTraceDirectory) { [IO.Directory]::Delete($selfTestOverLimitTraceDirectory, $true) }
    }
    $selfTestTraceDirectory = Join-Path $selfTestTraceRoot ('sakuracode-startup-trace-selftest-' + [Guid]::NewGuid().ToString('N'))
    try {
        [IO.Directory]::CreateDirectory($selfTestTraceDirectory) | Out-Null
        $traceFile = Join-Path $selfTestTraceDirectory 'startup-trace-123.jsonl'
        $traceLines = @(
            '{"schemaVersion":1,"qpc":1000,"frequency":1000,"pid":123,"tid":7,"role":"editor","event":"layout_begin","value1":0,"value2":0,"detail":""}',
            '{"schemaVersion":1,"qpc":1100,"frequency":1000,"pid":123,"tid":7,"role":"editor","event":"layout_complete","value1":0,"value2":0,"detail":""}',
            '{"schemaVersion":1,"qpc":1105,"frequency":1000,"pid":123,"tid":7,"role":"editor","event":"startup_draw_show_begin","value1":0,"value2":0,"detail":""}',
            '{"schemaVersion":1,"qpc":1115,"frequency":1000,"pid":123,"tid":7,"role":"editor","event":"startup_draw_show_end","value1":1,"value2":0,"detail":""}',
            '{"schemaVersion":1,"qpc":1125,"frequency":1000,"pid":124,"tid":8,"role":"control","event":"control_ready_event_begin","value1":0,"value2":0,"detail":""}',
            '{"schemaVersion":1,"qpc":1130,"frequency":1000,"pid":124,"tid":8,"role":"control","event":"control_wait_end","value1":0,"value2":0,"detail":""}',
            '{"schemaVersion":1,"qpc":1150,"frequency":1000,"pid":123,"tid":7,"role":"editor","event":"control_wait_end","value1":0,"value2":0,"detail":""}',
            '{"schemaVersion":1,"qpc":1160,"frequency":1000,"pid":124,"tid":8,"role":"control","event":"control_ready_event_end","value1":1,"value2":0,"detail":""}',
            '{"schemaVersion":1,"qpc":1175,"frequency":1000,"pid":123,"tid":7,"role":"editor","event":"editor_ready_event_begin","value1":0,"value2":0,"detail":""}',
            '{"schemaVersion":1,"qpc":1200,"frequency":1000,"pid":124,"tid":8,"role":"control","event":"editor_wait_end","value1":0,"value2":0,"detail":""}',
            '{"schemaVersion":1,"qpc":1210,"frequency":1000,"pid":123,"tid":7,"role":"editor","event":"editor_ready_event_end","value1":1,"value2":0,"detail":""}',
            '{"schemaVersion":1,"qpc":1211,"frequency":1000,"pid":123,"tid":7,"role":"editor","event":"first_content_advance_width_summary","value1":7,"value2":11,"detail":""}',
            '{"schemaVersion":1,"qpc":1212,"frequency":1000,"pid":123,"tid":7,"role":"editor","event":"first_content_draw_width_summary","value1":5,"value2":3,"detail":""}',
            '{"schemaVersion":1,"qpc":1213,"frequency":1000,"pid":123,"tid":7,"role":"editor","event":"first_content_text_output_summary","value1":2,"value2":3,"detail":""}',
            '{"schemaVersion":1,"qpc":1214,"frequency":1000,"pid":123,"tid":7,"role":"editor","event":"first_content_text_volume_summary","value1":11,"value2":9,"detail":""}',
            '{"schemaVersion":1,"qpc":1215,"frequency":1000,"pid":123,"tid":7,"role":"editor","event":"first_content_text_block_summary","value1":8,"value2":9,"detail":""}',
            '{"schemaVersion":1,"qpc":1216,"frequency":1000,"pid":123,"tid":7,"role":"editor","event":"first_content_text_block_font_summary","value1":10,"value2":11,"detail":""}',
            '{"schemaVersion":1,"qpc":1217,"frequency":1000,"pid":123,"tid":7,"role":"editor","event":"first_content_text_boundary_summary","value1":8589934595,"value2":17179869189,"detail":""}',
            '{"schemaVersion":1,"qpc":1218,"frequency":1000,"pid":123,"tid":7,"role":"editor","event":"first_content_text_scan_summary","value1":12,"value2":25769803783,"detail":""}',
            '{"schemaVersion":1,"qpc":1219,"frequency":1000,"pid":123,"tid":7,"role":"editor","event":"startup_draw_minimap_paint_summary","value1":4,"value2":2,"detail":""}',
            '{"schemaVersion":1,"qpc":1220,"frequency":1000,"pid":123,"tid":7,"role":"editor","event":"startup_draw_minimap_update_summary","value1":1,"value2":0,"detail":""}',
            '{"schemaVersion":1,"qpc":1221,"frequency":1000,"pid":123,"tid":7,"role":"editor","event":"first_content_nonblock_text_range_summary","value1":13,"value2":14,"detail":""}',
            '{"schemaVersion":1,"qpc":1222,"frequency":1000,"pid":123,"tid":7,"role":"editor","event":"first_content_nonblock_text_risk_summary","value1":15,"value2":16,"detail":""}',
            '{"schemaVersion":1,"qpc":1223,"frequency":1000,"pid":123,"tid":7,"role":"editor","event":"first_content_nonblock_text_other_summary","value1":17,"value2":18,"detail":""}',
            '{"schemaVersion":1,"qpc":1225,"frequency":1000,"pid":123,"tid":7,"role":"editor","event":"first_content_painted","value1":0,"value2":0,"detail":""}',
            '{not-json}'
        )
        [IO.File]::WriteAllLines($traceFile, $traceLines, (New-Object Text.UTF8Encoding($false)))
        $traceSummary = Get-StartupTraceSummary $selfTestTraceDirectory 1000 1000
        if (-not $traceSummary.collected -or $traceSummary.validRecordCount -ne 25 -or $traceSummary.invalidLineCount -ne 1) { throw 'Startup trace parsing self-test failed.' }
        if ($traceSummary.firstContentPainted.event -ne 'first_content_painted' -or $traceSummary.firstContentPaintedMs -ne 225) { throw 'First-content trace self-test failed.' }
        $layoutDuration = @($traceSummary.phaseDurations | Where-Object { $_.phase -eq 'layout' })
		$startupDrawShow = @($traceSummary.phaseDurations | Where-Object { $_.phase -eq 'startup_draw_show' })
        $controlReadyHandoff = @($traceSummary.phaseDurations | Where-Object { $_.phase -eq 'control_ready_handoff' })
        $editorReadyHandoff = @($traceSummary.phaseDurations | Where-Object { $_.phase -eq 'editor_ready_handoff' })
        if (-not $traceSummary.clockCompatible -or $traceSummary.majorPhases.Count -ne 25 -or $layoutDuration.Count -ne 1 -or $layoutDuration[0].durationMs -ne 100 -or $startupDrawShow.Count -ne 1 -or $startupDrawShow[0].durationMs -ne 10) { throw 'Startup trace clock self-test failed.' }
        if ($controlReadyHandoff.Count -ne 1 -or $controlReadyHandoff[0].durationMs -ne 25 -or $controlReadyHandoff[0].pid -ne 124 -or $controlReadyHandoff[0].endPid -ne 123 -or $controlReadyHandoff[0].role -ne 'control' -or $controlReadyHandoff[0].endRole -ne 'editor') { throw 'Control-ready handoff trace self-test failed.' }
        if ($editorReadyHandoff.Count -ne 1 -or $editorReadyHandoff[0].durationMs -ne 25 -or $editorReadyHandoff[0].pid -ne 123 -or $editorReadyHandoff[0].endPid -ne 124 -or $editorReadyHandoff[0].role -ne 'editor' -or $editorReadyHandoff[0].endRole -ne 'control') { throw 'Editor-ready handoff trace self-test failed.' }
        if ($traceSummary.firstContentPaintMetrics.advanceWidthMs -ne 7 -or $traceSummary.firstContentPaintMetrics.advanceWidthCalls -ne 11 -or $traceSummary.firstContentPaintMetrics.drawWidthMs -ne 5 -or $traceSummary.firstContentPaintMetrics.textOutputMs -ne 2 -or $traceSummary.firstContentPaintMetrics.drawnUtf16Units -ne 9 -or
            $traceSummary.firstContentPaintMetrics.textBlockCount -ne 8 -or $traceSummary.firstContentPaintMetrics.alternateFontBlockCount -ne 10 -or $traceSummary.firstContentPaintMetrics.maximumTextBlockUtf16Units -ne 11 -or
            $traceSummary.firstContentPaintMetrics.renderTypeBoundaryCount -ne 2 -or $traceSummary.firstContentPaintMetrics.lengthBoundaryCount -ne 3 -or $traceSummary.firstContentPaintMetrics.colorBoundaryCount -ne 4 -or $traceSummary.firstContentPaintMetrics.tailBoundaryCount -ne 5 -or
            $traceSummary.firstContentPaintMetrics.figureLookupCount -ne 12 -or $traceSummary.firstContentPaintMetrics.nonTextFigureCount -ne 6 -or $traceSummary.firstContentPaintMetrics.colorChangeCount -ne 7 -or
            $traceSummary.firstContentPaintMetrics.nonBlockCjkSymbolsAndPunctuationCount -ne 13 -or $traceSummary.firstContentPaintMetrics.nonBlockGeneralPunctuationCount -ne 14 -or
            $traceSummary.firstContentPaintMetrics.nonBlockCombiningOrVariationCount -ne 15 -or $traceSummary.firstContentPaintMetrics.nonBlockSurrogatePairCount -ne 16 -or
            $traceSummary.firstContentPaintMetrics.nonBlockLatinExtendedCount -ne 17 -or $traceSummary.firstContentPaintMetrics.nonBlockOtherBmpCount -ne 18) { throw 'First-content aggregate trace self-test failed.' }
        if ($traceSummary.startupMiniMapMetrics.paintMs -ne 4 -or $traceSummary.startupMiniMapMetrics.paintCount -ne 2 -or $traceSummary.startupMiniMapMetrics.immediateUpdateCount -ne 1) { throw 'Startup minimap aggregate trace self-test failed.' }
    }
    finally {
        if (Test-Path -LiteralPath $selfTestTraceDirectory) {
            [IO.Directory]::Delete($selfTestTraceDirectory, $true)
        }
    }
    return [ordered]@{
        selfTest = $true
        passed = $true
        warmNativeProcessSnapshotMs = [Math]::Round($snapshotWatch.Elapsed.TotalMilliseconds, 3)
        profileSidecarVerified = $true
        artifactBundleVerified = $true
        artifactBundleCleanupVerified = [bool]$bundleCleanupVerified
        artifactClosureVerified = $true
        jobContainmentSelfTestVerified = [bool]$jobSelfTestClosed
        jobQueryObservationSelfTestVerified = [bool]$jobQueryObservationSelfTestVerified
        jobQueryError234Retained = [bool]$jobQueryError234Retained

        jobQueryRetryBehaviorCorrected = [bool]$jobQueryRetryBehaviorCorrected
        jobQueryRetryContractSelfTestVerified = [bool]$jobQueryRetryContractSelfTestVerified
        processEnumerationRetryBehaviorCorrected = [bool]$processEnumerationRetryBehaviorCorrected
        processEnumerationRetryContractSelfTestVerified = [bool]$processEnumerationRetryContractSelfTestVerified
        processEnumerationContractSelfTestVerified = [bool]$processEnumerationRetryContractSelfTestVerified
        processEnumerationAggregateSelfTestVerified = [bool]$successfulEnumerationAggregateVerified
        processEnumerationFirstFailureRetainedVerified = [bool]$firstFailureEnumerationRetainedVerified
        processEnumerationBoundedCountsVerified = [bool]$boundedEnumerationCountsVerified
        processEnumerationMalformedEnvelopeSelfTestVerified = [bool]$processEnumerationMalformedEnvelopeSelfTestVerified
        processEnumerationFailedEnvelopeSelfTestVerified = [bool]$processEnumerationFailedEnvelopeSelfTestVerified
        processEnumerationStrictMetadataSelfTestVerified = [bool]$processEnumerationStrictMetadataSelfTestVerified
        processEnumerationPidZeroSelfTestVerified = [bool]$processEnumerationPidZeroSelfTestVerified
        trackedSweepFirstFailureRetainedVerified = [bool]$trackedFirstFailureRetainedVerified
        trackedSweepFailureEnumsVerified = [bool]$trackedFailureEnumsVerified
        trackedSweepBoundedCountsVerified = [bool]$boundedTrackedCountsVerified
        trackedIdentityDisappearedSelfTestVerified = [bool]$identityDisappearedVerified
        trackedIdentityStillPresentSelfTestVerified = [bool]$identityStillPresentVerified
        trackedIdentityCensusUnavailableSelfTestVerified = [bool]$identityUnavailableVerified
        trackedIdentityNoObservationSelfTestVerified = [bool]$identityNoObservationVerified
        trackedIdentityMalformedProbeSelfTestVerified = [bool]$identityMalformedProbeVerified
        trackedIdentityMalformedFreshCensusSelfTestVerified = [bool]$freshMalformedVerified
        jobIdentityDisappearedTelemetryDisabledSelfTestVerified = [bool]$jobIdentityDisappearedTelemetryDisabledVerified
        jobIdentityStillPresentTelemetryDisabledSelfTestVerified = [bool]$jobIdentityStillPresentTelemetryDisabledVerified
        jobIdentityCensusUnavailableTelemetryDisabledSelfTestVerified = [bool]$jobIdentityCensusUnavailableTelemetryDisabledVerified
        jobIdentityMalformedFreshCensusTelemetryDisabledSelfTestVerified = [bool]$jobIdentityMalformedFreshCensusTelemetryDisabledVerified
        jobIdentityTelemetryDisabledSelfTestVerified = [bool]$jobIdentityTelemetryDisabledSelfTestVerified
        jobIdentityProductionPathSelfTestVerified = [bool]$jobIdentityProductionPathSelfTestVerified
        jobIdentityAcceptedDisappearanceSelfTestVerified = [bool]$jobIdentityAcceptedVerified
        jobIdentityStillPresentSelfTestVerified = [bool]$jobIdentityStillPresentVerified
        gracefulIdentityFallbackSelfTestVerified = [bool]$gracefulIdentityFallbackSelfTestVerified
        jobIdentityUnavailableSelfTestVerified = [bool]$jobIdentityUnavailableVerified
        jobIdentityMalformedFreshQuerySelfTestVerified = [bool]$jobIdentityMalformedVerified
        jobIdentityInvocationExceptionSelfTestVerified = [bool]$jobIdentityInvocationExceptionVerified
        jobIdentityFreshQueryExceptionSelfTestVerified = [bool]$jobIdentityFreshQueryExceptionVerified
        jobIdentityConversionFailureSelfTestVerified = [bool]$jobIdentityConversionFailureVerified
        jobIdentityContradictorySuccessSelfTestVerified = [bool]$jobIdentityContradictorySuccessVerified
        jobIdentityMalformedFreshQueryCasesSelfTestVerified = [bool]$jobIdentityMalformedFreshQueryCasesVerified
        jobIdentityMalformedFreshQueryMetadataCasesSelfTestVerified = [bool]$jobIdentityMalformedFreshQueryMetadataCasesVerified
        descendantCreationOrderSelfTestVerified = [bool]$descendantCreationOrderSelfTestVerified
        affinityHistoricalCountsSelfTestVerified = [bool]$affinityHistoricalCountsVerified
        affinityCurrentSetSelfTestVerified = [bool]$affinityCurrentSetSelfTestVerified
        affinityInvalidCurrentSetSelfTestVerified = [bool]$affinityInvalidCurrentSetSelfTestVerified
        affinityExpiredHistoricalExcludedSelfTestVerified = [bool]($affinityCurrentSetSelfTestVerified -and
            -not ($affinityPlanIdsSelfTest -contains 5))
        realProcessEnumerationSelfTestVerified = [bool]$realProcessEnumerationSelfTestVerified
        realMultiMemberJobQuerySelfTestVerified = [bool]$realMultiMemberJobQuerySelfTestVerified
        realMultiMemberJobTerminationSelfTestVerified = [bool]$realMultiMemberJobTerminationSelfTestVerified
        strictTerminalJobQuerySelfTestVerified = [bool]$strictTerminalJobQuerySelfTestVerified
        strictGracefulObservationSelfTestVerified = [bool]$strictGracefulObservationSelfTestVerified
        strictNativeEnvelopeSelfTestVerified = [bool]$strictNativeEnvelopeSelfTestVerified
        boundedRejectedReconciliationSelfTestVerified = [bool]$boundedRejectedReconciliationSelfTestVerified
        openProcessIdentityEscalationSelfTestVerified = [bool]$openProcessIdentityEscalationSelfTestVerified
        verifiedExplicitTerminationSelfTestVerified = [bool]$verifiedExplicitTerminationSelfTestVerified
        containmentFailureBranchesSelfTestVerified = [bool]$containmentFailureBranchesSelfTestVerified
        containmentEnumAndPayloadSelfTestVerified = [bool]$containmentEnumAndPayloadSelfTestVerified
        externalZeroWithoutJobProofRejected = [bool]$externalZeroWithoutJobProofRejected
        cleanupObservationSelfTestVerified = [bool]$cleanupObservationSelfTestVerified
        failedQueryCleanupRemainsUnverified = [bool]$failedQueryCleanupRemainsUnverified
        cleanupErrorCountSelfTestVerified = [bool]$cleanupErrorCountSelfTestVerified
        gracefulTerminalCompletionSelfTestVerified = [bool]$gracefulTerminalCompletionSelfTestVerified
        readinessInputIdleOptionalSelfTestVerified = [bool]$readinessInputIdleOptionalSelfTestVerified
        workingDirectorySelfTestVerified = $true
        startupDiagnosticsSchemaVerified = [bool]$diagnosticSchemaVerified
        startupDiagnosticBoundsVerified = [bool]$diagnosticBoundsVerified
        startupDiagnosticWindowClassificationVerified = [bool]$windowClassificationVerified
        startupDiagnosticSubstageSelfTestVerified = [bool]$diagnosticCheckpointSubstageSelfTestVerified
        startupDiagnosticProductionIdentityStillPresentSelfTestVerified = [bool]$diagnosticProductionIdentityStillPresentSelfTestVerified
        startupDiagnosticProductionMalformedIdentitySelfTestVerified = [bool]$diagnosticMalformedIdentitySelfTestVerified
        startupDiagnosticProductionDisappearanceWindowSelfTestVerified = [bool]$diagnosticProductionDisappearanceWindowSelfTestVerified
        noGuiLaunch = $true
        timestampUtc = [DateTime]::UtcNow.ToString('o')
    }
}

if ($LibraryOnly) {
    return
}

try {
    if ($SelfTest) {
        Invoke-SelfTest | ConvertTo-Json -Compress
        exit 0
    }
    if ([string]::IsNullOrWhiteSpace($SakuraExe)) { throw '-SakuraExe is required unless -SelfTest is used.' }
    $exePath = (Resolve-Path -LiteralPath $SakuraExe).Path
    $documentPath = (Resolve-Path -LiteralPath $SampleMarkdown).Path
    if (-not (Test-Path -LiteralPath $exePath -PathType Leaf)) { throw "SakuraExe is not a file: $exePath" }
    if (-not (Test-Path -LiteralPath $documentPath -PathType Leaf)) { throw "SampleMarkdown is not a file: $documentPath" }
    $outputPath = [IO.Path]::GetFullPath($OutputDirectory)
    [IO.Directory]::CreateDirectory($outputPath) | Out-Null
    $runId = '{0}-{1}' -f [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff'), ([Guid]::NewGuid().ToString('N').Substring(0, 8))
    $reportPath = Join-Path $outputPath ("startup-performance-$runId.json")
    $existing = @(Get-ProcessesForImagePath $exePath)
    if ($existing.Count -gt 0) { throw "Refusing to run while SakuraExe is already running (PID(s): $($existing.Id -join ', '))." }

    $sourceArtifact = Get-StartupFileIdentity $exePath
    $documentInfo = Get-Item -LiteralPath $documentPath
    $exeInfo = Get-Item -LiteralPath $exePath
    $lines = @(Get-Content -LiteralPath $documentPath).Count
    if ($lines -le 30) {
        throw "SampleMarkdown must contain at least 31 physical lines so document readiness cannot match Sakura's initial scrollbar placeholder. Actual: $lines."
    }
    $bundle = $null
    $bundleVerification = $null
    $bundleCleanupVerified = $false
    $bundleName = 'startup-probe-bundle-{0}' -f $runId
    $bundle = New-StartupArtifactBundle $exePath $outputPath $bundleName
    $launchExePath = $bundle.executablePath
    $exeDirectory = $bundle.bundlePath
    try {
        $runs = New-Object Collections.Generic.List[object]
        for ($iteration = 1; $iteration -le $Iterations; $iteration++) {
            [void](Assert-StartupArtifactBundleUnchanged $bundle)
            $profileName = 'startup-probe-{0}' -f ([Guid]::NewGuid().ToString('N'))
            $profilePath = Join-Path $exeDirectory $profileName
            Assert-OwnedProfilePath $profilePath $exeDirectory $profileName
            if (Test-Path -LiteralPath $profilePath) { throw "Generated profile directory already exists: $profilePath" }
            $iterationRuns = New-Object Collections.Generic.List[object]
            $profileCleanupError = $null
            try {
                $freshImage = Join-Path $outputPath ('startup-performance-{0}-fresh-iteration-{1}.png' -f $runId, $iteration)
                $freshTraceDirectory = New-StartupTraceDirectory $outputPath $runId $iteration 'fresh'
                $freshRun = Invoke-StartupMeasurement 'fresh' $iteration $launchExePath $documentPath $lines $profileName $profilePath $exeDirectory ($CaptureScreenshot -and $iteration -eq 1) $freshImage $freshTraceDirectory
                $iterationRuns.Add($freshRun)
                $runs.Add($freshRun)
                if ($CompareExistingProfile) {
                    $existingImage = Join-Path $outputPath ('startup-performance-{0}-existing-profile-iteration-{1}.png' -f $runId, $iteration)
                    $existingTraceDirectory = New-StartupTraceDirectory $outputPath $runId $iteration 'existing-profile'
                    $existingRun = Invoke-StartupMeasurement 'existingProfile' $iteration $launchExePath $documentPath $lines $profileName $profilePath $exeDirectory ($CaptureScreenshot -and $iteration -eq 1) $existingImage $existingTraceDirectory
                    $iterationRuns.Add($existingRun)
                    $runs.Add($existingRun)
                }
            }
            finally {
                try {
                    Remove-OwnedProfile $profilePath $exeDirectory $profileName
                    if (Test-Path -LiteralPath $profilePath) { throw "Profile directory survived cleanup: $profilePath" }
                }
                catch { $profileCleanupError = $_.Exception.Message }

                foreach ($run in $iterationRuns) {
                    $run.profileCleanupVerified = $null -eq $profileCleanupError
                    $run.cleanupVerified = $run.processCleanupVerified -and $run.profileCleanupVerified
                    if (-not $run.cleanupVerified) {
                        $run.success = $false
                        if ($null -ne $profileCleanupError) {
                            $message = "Profile cleanup failed: $profileCleanupError"
                            if ([string]::IsNullOrEmpty([string]$run.error)) { $run.error = $message }
                            else { $run.error = "$($run.error) $message" }
                        }
                    }
                }
                if ($iterationRuns.Count -eq 0 -and $null -ne $profileCleanupError) { throw $profileCleanupError }
            }
        }
        $bundleVerification = Assert-StartupArtifactBundleUnchanged $bundle
    }
    finally {
        if ($null -ne $bundle) {
            try {
                Remove-StartupArtifactBundle $bundle
                $bundleCleanupVerified = -not (Test-Path -LiteralPath $bundle.bundlePath)
            }
            catch { $bundleCleanupVerified = $false }
        }
    }
    if ($null -eq $bundleVerification) { throw 'The startup artifact bundle could not be verified.' }
    $bundleVerification.cleanupVerified = [bool]$bundleCleanupVerified
    $metricNames = @('processApiReturnMs', 'topLevelHwndMs', 'visibleMs', 'dwmFlushMs', 'captionReadyMs', 'inputIdleMs', 'documentReadyMs')
    $summaries = foreach ($condition in @($runs | Select-Object -ExpandProperty condition -Unique)) {
        $conditionRuns = @($runs | Where-Object { $_.condition -eq $condition })
        foreach ($metric in $metricNames) {
            $successful = @($conditionRuns | Where-Object { $_.success -and $_.cleanupVerified -and $null -ne $_.$metric })
            [ordered]@{
                condition = $condition
                metric = $metric
                statistics = Get-Statistics ([double[]]@($successful | ForEach-Object { $_.$metric }))
                successfulRuns = $successful.Count
                totalRuns = $conditionRuns.Count
            }
        }
    }
    $os = Get-CimInstance Win32_OperatingSystem
    $cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
    $exeVersion = [Diagnostics.FileVersionInfo]::GetVersionInfo($exePath)
    $report = [ordered]@{
        generatedAtUtc = [DateTime]::UtcNow.ToString('o'); scriptVersion = '1.14'; runId = $runId; reportPath = $reportPath
        cleanupVerified = -not (@($runs | Where-Object { -not $_.cleanupVerified }).Count) -and [bool]$bundleCleanupVerified
        configuration = [ordered]@{ sakuraExe = $exePath; sampleMarkdown = $documentPath; iterations = $Iterations; compareExistingProfile = [bool]$CompareExistingProfile; captureScreenshot = [bool]$CaptureScreenshot; measureDwmFlush = [bool]$MeasureDwmFlush; startupTraceEnvironmentVariable = 'SAKURA_STARTUP_TRACE_DIR'; outputDirectory = $outputPath; profilePolicy = $script:StartupProfileSidecarContract }
        input = [ordered]@{ bytes = [int64]$documentInfo.Length; lines = $lines; sha256 = (Get-Sha256 $documentPath) }
        artifactBundle = [ordered]@{
            sourceHashBefore = $bundleVerification.sourceHashBefore
            sourceHashAfter = $bundleVerification.sourceHashAfter
            sourceSizeBefore = [UInt64]$bundleVerification.sourceSizeBefore
            sourceSizeAfter = [UInt64]$bundleVerification.sourceSizeAfter
            sourceUnchanged = [bool]$bundleVerification.sourceUnchanged
            copiedHashBefore = $bundleVerification.copiedHashBefore
            copiedHashAfter = $bundleVerification.copiedHashAfter
            copiedSizeBefore = [UInt64]$bundleVerification.copiedSizeBefore
            copiedSizeAfter = [UInt64]$bundleVerification.copiedSizeAfter
            copiedUnchanged = [bool]$bundleVerification.copiedUnchanged
            sidecarContract = $bundle.sidecar.contract
            sidecarSha256 = $bundleVerification.sidecarSha256
            sidecarSizeBytes = [UInt64]$bundleVerification.sidecarSizeBytes
            sidecarMultiUser = [int]$bundle.sidecar.multiUser
            sidecarVerified = [bool]$bundleVerification.sidecarVerified
            cleanupVerified = [bool]$bundleCleanupVerified
        }
        environment = [ordered]@{
            os = $os.Caption; osBuild = $os.BuildNumber; osVersion = [Environment]::OSVersion.Version.ToString()
            cpu = $cpu.Name; logicalProcessors = $cpu.NumberOfLogicalProcessors
            memoryGiB = [Math]::Round($os.TotalVisibleMemorySize / 1MB, 1)
            exeVersion = $exeVersion.FileVersion; exeProductVersion = $exeVersion.ProductVersion
            exeBytes = [int64]$exeInfo.Length; exeLastWriteTimeUtc = $exeInfo.LastWriteTimeUtc.ToString('o')
        }
        conditions = @('fresh') + $(if ($CompareExistingProfile) { @('existingProfile') } else { @() })
        runs = @($runs | ForEach-Object { $_ })
        summaries = @($summaries)
    }
    $json = $report | ConvertTo-Json -Depth 8
    [IO.File]::WriteAllText($reportPath, $json, (New-Object Text.UTF8Encoding($false)))
    Write-Output $json
    if (@($runs | Where-Object { -not $_.success -or -not $_.cleanupVerified }).Count -gt 0) { exit 1 }
    exit 0
}
catch {
    Write-Error ("{0}`n{1}" -f $_.Exception.Message, $_.ScriptStackTrace)
    exit 1
}
