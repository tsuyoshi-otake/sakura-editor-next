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
    public StartupProbeProcess Identity;
}

public sealed class StartupProbeJobResult
{
    public IntPtr Handle;
    public bool Succeeded;
    public int ErrorCode;
    public int[] ProcessIds;
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
    private const int ERROR_BAD_LENGTH = 24;
    private const int ERROR_INVALID_PARAMETER = 87;
    private const int ERROR_NOT_FOUND = 1168;
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

    public static StartupProbeProcessEntry[] GetProcessEntries()
    {
        var result = new ArrayList();
        IntPtr snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return (StartupProbeProcessEntry[])result.ToArray(typeof(StartupProbeProcessEntry));
        try {
            var entry = new PROCESSENTRY32();
            entry.Size = unchecked((uint)Marshal.SizeOf(typeof(PROCESSENTRY32)));
            if (!Process32FirstW(snapshot, ref entry)) return (StartupProbeProcessEntry[])result.ToArray(typeof(StartupProbeProcessEntry));
            do {
                result.Add(new StartupProbeProcessEntry {
                    ProcessId = unchecked((int)entry.ProcessId),
                    ParentProcessId = unchecked((int)entry.ParentProcessId),
                    ImageName = entry.ExeFile
                });
            } while (Process32NextW(snapshot, ref entry));
        }
        finally { CloseHandle(snapshot); }
        return (StartupProbeProcessEntry[])result.ToArray(typeof(StartupProbeProcessEntry));
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
        var result = new StartupProbeProcessIdentityResult { Succeeded = false, ErrorCode = 0, Identity = null };
        IntPtr process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, unchecked((uint)processId));
        if (process == IntPtr.Zero) {
            result.ErrorCode = Marshal.GetLastWin32Error();
            return result;
        }
        try {
            var path = new StringBuilder(32768);
            int pathLength = path.Capacity;
            FILETIME creation, exit, kernel, user;
            if (!QueryFullProcessImageNameW(process, 0, path, ref pathLength)) {
                result.ErrorCode = Marshal.GetLastWin32Error();
                return result;
            }
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
            return result;
        }
        finally { CloseHandle(process); }
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

    public static StartupProbeJobResult QueryJobProcessIds(IntPtr job)
    {
        var result = new StartupProbeJobResult { Handle = job, Succeeded = false, ErrorCode = 0, ProcessIds = new int[0] };
        if (job == IntPtr.Zero) { result.ErrorCode = 6; return result; }
        uint required = 0;
        QueryInformationJobObject(job, JobObjectBasicProcessIdList, IntPtr.Zero, 0, out required);
        int lastError = Marshal.GetLastWin32Error();
        if (required < 8 && lastError != ERROR_INSUFFICIENT_BUFFER && lastError != ERROR_BAD_LENGTH) {
            result.ErrorCode = lastError;
            return result;
        }
        // The API reports ERROR_BAD_LENGTH and a zero return length for an
        // empty job on Windows.  The two ULONG header fields are followed by
        // a variable-length ULONG_PTR array; passing only the header back to
        // the second query is rejected with ERROR_BAD_LENGTH on x64.  Always
        // reserve room for the first array slot, including the empty case.
        uint minimumSize = unchecked((uint)(8 + IntPtr.Size));
        if (required < minimumSize) required = minimumSize;
        for (int attempt = 0; attempt < 3; ++attempt) {
            if (required > 1024 * 1024) { result.ErrorCode = 8; return result; }
            IntPtr buffer = Marshal.AllocHGlobal(unchecked((int)required));
            try {
                uint returned;
                if (!QueryInformationJobObject(job, JobObjectBasicProcessIdList, buffer, required, out returned)) {
                    int error = Marshal.GetLastWin32Error();
                    if ((error == ERROR_INSUFFICIENT_BUFFER || error == ERROR_BAD_LENGTH) && returned > required) {
                        required = returned;
                        continue;
                    }
                    result.ErrorCode = error;
                    return result;
                }
                uint assigned = unchecked((uint)Marshal.ReadInt32(buffer, 0));
                uint listed = unchecked((uint)Marshal.ReadInt32(buffer, 4));
                ulong capacity = unchecked(((ulong)required - 8UL) / (ulong)IntPtr.Size);
                if (listed > assigned || (ulong)listed > capacity) {
                    result.ErrorCode = ERROR_BAD_LENGTH;
                    return result;
                }
                var ids = new int[unchecked((int)listed)];
                for (uint i = 0; i < listed; ++i) {
                    IntPtr value = Marshal.ReadIntPtr(buffer, 8 + unchecked((int)(i * (uint)IntPtr.Size)));
                    long processId = value.ToInt64();
                    if (processId <= 0 || processId > Int32.MaxValue) { result.ErrorCode = ERROR_BAD_LENGTH; return result; }
                    ids[unchecked((int)i)] = unchecked((int)processId);
                }
                result.ProcessIds = ids;
                result.Succeeded = true;
                return result;
            }
            finally { Marshal.FreeHGlobal(buffer); }
        }
        result.ErrorCode = ERROR_INSUFFICIENT_BUFFER;
        return result;
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

function Get-AffinityMetadata($Probe) {
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
    }
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

function Get-ProcessSnapshot($Owned, [int[]]$SeedIds = @()) {
    $entries = @([NativeStartupProbe]::GetProcessEntries())
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
        $identityProbe = [NativeStartupProbe]::QueryProcessIdentity([int]$entry.ProcessId, [int]$entry.ParentProcessId)
        if (-not $identityProbe.Succeeded -or $null -eq $identityProbe.Identity) {
            # A process can disappear between Toolhelp32 and OpenProcess.  That
            # race is benign only when a fresh snapshot proves it disappeared;
            # an entry that remains present but cannot be identified is never
            # silently omitted from a containment decision.
            $stillPresent = @([NativeStartupProbe]::GetProcessEntries() | Where-Object { [int]$_.ProcessId -eq [int]$entry.ProcessId }).Count -gt 0
            if ($stillPresent) {
                throw "Could not verify the identity of relevant process $($entry.ProcessId) (Win32 $($identityProbe.ErrorCode))."
            }
            continue
        }
        $record = Convert-ProcessIdentity $identityProbe.Identity
        $result[$record.Id] = $record
    }
    return $result
}

function Get-ProcessesForImagePath([string]$ImagePath) {
    $imageName = [IO.Path]::GetFileName($ImagePath)
    $result = @()
    foreach ($entry in @([NativeStartupProbe]::GetProcessEntries())) {
        if (-not [string]::Equals([string]$entry.ImageName, $imageName, [StringComparison]::OrdinalIgnoreCase)) { continue }
        $identityProbe = [NativeStartupProbe]::QueryProcessIdentity([int]$entry.ProcessId, [int]$entry.ParentProcessId)
        if (-not $identityProbe.Succeeded -or $null -eq $identityProbe.Identity) {
            # A process disappearing between Toolhelp and OpenProcess is benign for
            # this read-only sweep.  An entry which is still present but cannot be
            # identified is never treated as clean.
            $stillPresent = @([NativeStartupProbe]::GetProcessEntries() | Where-Object { [int]$_.ProcessId -eq [int]$entry.ProcessId }).Count -gt 0
            if ($stillPresent) { throw "Could not verify the identity of matching process $($entry.ProcessId) (Win32 $($identityProbe.ErrorCode))." }
            continue
        }
        $record = Convert-ProcessIdentity $identityProbe.Identity
        if (Test-SamePath $record.ImagePath $ImagePath) { $result += $record }
    }
    return $result
}

function Get-JobProcessRecords([IntPtr]$Job, $Owned) {
    if ($Job -eq [IntPtr]::Zero) { throw 'A run-owned job handle is required.' }
    $query = [NativeStartupProbe]::QueryJobProcessIds($Job)
    if (-not $query.Succeeded -or $null -eq $query.ProcessIds) {
        throw "Could not enumerate run-owned job processes (Win32 $($query.ErrorCode))."
    }
    $entries = @([NativeStartupProbe]::GetProcessEntries())
    $records = New-Object Collections.Generic.List[object]
    foreach ($processId in @($query.ProcessIds)) {
        $entry = @($entries | Where-Object { [int]$_.ProcessId -eq [int]$processId } | Select-Object -First 1)
        if ($entry.Count -eq 0) {
            # The process may have exited after the job query.  Re-query the job
            # once before declaring an identity gap; an unobservable member is not
            # a clean result.
            $retry = [NativeStartupProbe]::QueryJobProcessIds($Job)
            if (-not $retry.Succeeded -or @($retry.ProcessIds | Where-Object { [int]$_ -eq [int]$processId }).Count -gt 0) {
                throw "Could not observe the identity of run-owned job process $processId."
            }
            continue
        }
        $identityProbe = [NativeStartupProbe]::QueryProcessIdentity([int]$entry[0].ProcessId, [int]$entry[0].ParentProcessId)
        if (-not $identityProbe.Succeeded -or $null -eq $identityProbe.Identity) {
            throw "Could not verify the identity of run-owned job process $processId (Win32 $($identityProbe.ErrorCode))."
        }
        $record = Convert-ProcessIdentity $identityProbe.Identity
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

function Update-OwnedProcesses($Owned, $Snapshot) {
    $changed = $true
    while ($changed) {
        $changed = $false
        foreach ($candidate in @($Snapshot.Values)) {
            if ($Owned.ContainsKey($candidate.Id)) { continue }
            if (-not $Owned.ContainsKey($candidate.ParentId)) { continue }
            $parent = $Owned[$candidate.ParentId]
            if (-not (Test-ProcessIdentity $parent $Snapshot)) { continue }
            $Owned[$candidate.Id] = $candidate
            $changed = $true
        }
    }
}

function Get-LiveOwnedProcesses($Owned, [IntPtr]$Job = [IntPtr]::Zero) {
    if ($Job -ne [IntPtr]::Zero) {
        return @(Get-JobProcessRecords $Job $Owned)
    }
    $snapshot = Get-ProcessSnapshot $Owned
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
    return $null -ne $State.captionReadyMs -and $State.inputIdleReached -and $null -ne $State.documentReadyMs
}

function Get-StartupReadinessMissingMilestones($State, [int]$ExpectedLineCount) {
    $missing = New-Object Collections.Generic.List[string]
    if ($null -eq $State.captionReadyMs) { $missing.Add('visible document caption') }
    if (-not $State.inputIdleReached) { $missing.Add('input idle') }
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
    $summary = [ordered]@{
        enabled = $true
        collected = $false
        directory = $TraceDirectory
        launchQpc = $LaunchQpc
        launchFrequency = $LaunchFrequency
        files = @()
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

    $records = New-Object Collections.Generic.List[object]
    foreach ($traceFile in @(Get-ChildItem -LiteralPath $TraceDirectory -File -Filter 'startup-trace-*.jsonl' | Sort-Object Name)) {
        $summary.files += $traceFile.Name
        $lineNumber = 0
        foreach ($line in @(Get-Content -LiteralPath $traceFile.FullName)) {
            $lineNumber++
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
                $records.Add($record)
            }
            catch {
                $summary.invalidLineCount++
                $summary.parseErrors += [ordered]@{ file = $traceFile.Name; line = $lineNumber; error = $_.Exception.Message }
            }
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

function Get-TrackedOwnedProcesses($Owned) {
    $entries = @([NativeStartupProbe]::GetProcessEntries())
    $records = New-Object Collections.Generic.List[object]
    foreach ($record in @($Owned.Values)) {
        $entry = @($entries | Where-Object { [int]$_.ProcessId -eq [int]$record.Id } | Select-Object -First 1)
        if ($entry.Count -eq 0) { continue }
        $identityProbe = [NativeStartupProbe]::QueryProcessIdentity([int]$entry[0].ProcessId, [int]$entry[0].ParentProcessId)
        if (-not $identityProbe.Succeeded -or $null -eq $identityProbe.Identity) {
            throw "Could not verify the identity of tracked process $($record.Id) after job cleanup (Win32 $($identityProbe.ErrorCode))."
        }
        $current = Convert-ProcessIdentity $identityProbe.Identity
        if ($current.Creation -eq $record.Creation -and (Test-SamePath $current.ImagePath $record.ImagePath)) {
            [void]$records.Add($current)
        }
    }
    return $records.ToArray()
}

function Stop-OwnedProcesses($Owned, [IntPtr]$Job = [IntPtr]::Zero, [string]$ExecutablePath = $null) {
    $closeWatch = [Diagnostics.Stopwatch]::StartNew()
    $jobQuerySucceeded = $Job -eq [IntPtr]::Zero
    $jobCloseSucceeded = $Job -eq [IntPtr]::Zero
    $graceful = $false
    $cleanupErrors = New-Object Collections.Generic.List[string]
    $knownJobProcesses = New-Object Collections.Generic.List[object]
    try {
        if ($Job -ne [IntPtr]::Zero) {
            foreach ($record in @(Get-JobProcessRecords $Job $Owned)) { [void]$knownJobProcesses.Add($record) }
            $jobQuerySucceeded = $true
        }
        else {
            $snapshot = Get-ProcessSnapshot $Owned
            Update-OwnedProcesses $Owned $snapshot
            foreach ($record in @($Owned.Values)) { [void]$knownJobProcesses.Add($record) }
        }

        # Ask parents to close before descendants.  This keeps the control process
        # from creating more work while the editor window is being dismissed.
        $windows = @([NativeStartupProbe]::GetTopLevelWindows() | Where-Object { $Owned.ContainsKey([int]$_.ProcessId) })
        $windowRecords = foreach ($window in $windows) {
            $record = $Owned[[int]$window.ProcessId]
            [pscustomobject]@{ Window = $window; Depth = Get-OwnedProcessDepth $record $Owned; Id = [int]$record.Id }
        }
        foreach ($windowRecord in @($windowRecords | Sort-Object Depth, Id)) {
            [void][NativeStartupProbe]::RequestClose($windowRecord.Window.Handle)
        }

        $remainingGraceMs = [Math]::Max(1, $closeTimeoutMs - [int]$closeWatch.ElapsedMilliseconds)
        if ($Job -ne [IntPtr]::Zero) {
            $graceful = Wait-WithPoll { @(Get-LiveOwnedProcesses $Owned $Job).Count -eq 0 } $remainingGraceMs
        }
        else {
            $graceful = Wait-WithPoll { @(Get-LiveOwnedProcesses $Owned).Count -eq 0 } $remainingGraceMs
        }
    }
    catch { [void]$cleanupErrors.Add($_.Exception.Message) }
    finally {
        if ($Job -ne [IntPtr]::Zero) {
            $close = [NativeStartupProbe]::CloseKillOnCloseJob($Job)
            $jobCloseSucceeded = [bool]$close.Succeeded
            if (-not $jobCloseSucceeded) { [void]$cleanupErrors.Add("Could not close the run-owned job (Win32 $($close.ErrorCode)).") }
        }
    }

    # KILL_ON_JOB_CLOSE is the containment boundary.  Do not force-terminate by
    # PID here: a PID can be reused, while closing this job can only affect members
    # that were assigned by this invocation.
    $settled = $false
    $tracked = @()
    $pathMatches = @()
    $finalPathSweepVerified = $false
    while ($closeWatch.ElapsedMilliseconds -lt $closeTimeoutMs) {
        try {
            $tracked = @(Get-TrackedOwnedProcesses $Owned)
            if (-not [string]::IsNullOrWhiteSpace($ExecutablePath)) {
                $pathMatches = @(Get-ProcessesForImagePath $ExecutablePath)
                $finalPathSweepVerified = $true
            }
            if ($tracked.Count -eq 0 -and $pathMatches.Count -eq 0) { $settled = $true; break }
        }
        catch {
            [void]$cleanupErrors.Add($_.Exception.Message)
            break
        }
        Start-Sleep -Milliseconds $pollIntervalMs
    }
    if (-not $settled) {
        try {
            $tracked = @(Get-TrackedOwnedProcesses $Owned)
            if (-not [string]::IsNullOrWhiteSpace($ExecutablePath)) {
                $pathMatches = @(Get-ProcessesForImagePath $ExecutablePath)
                $finalPathSweepVerified = $true
            }
        }
        catch { [void]$cleanupErrors.Add($_.Exception.Message) }
    }
    $survivorsById = @{}
    foreach ($record in @($tracked + $pathMatches)) { $survivorsById[[int]$record.Id] = $record }
    if (-not $jobQuerySucceeded) { [void]$cleanupErrors.Add('Run-owned job membership was not verified.') }
    if (-not $jobCloseSucceeded) { [void]$cleanupErrors.Add('Run-owned job containment was not closed successfully.') }
    if (-not $finalPathSweepVerified) { [void]$cleanupErrors.Add('The final exact executable-path sweep was not completed.') }
    return [pscustomobject][ordered]@{
        survivors = @($survivorsById.Values)
        graceful = [bool]$graceful
        jobQuerySucceeded = [bool]$jobQuerySucceeded
        jobCloseSucceeded = [bool]$jobCloseSucceeded
        finalPathSweepVerified = [bool]$finalPathSweepVerified
        error = if ($cleanupErrors.Count -eq 0) { $null } else { ($cleanupErrors.ToArray() -join ' ') }
    }
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
        }
        startupTrace = $null
        screenshotPath = $null; success = $false; error = $null
        processCleanupVerified = $false; profileCleanupVerified = $false; cleanupVerified = $false
        jobQuerySucceeded = $false; jobCloseSucceeded = $false; finalPathSweepVerified = $false; containmentVerified = $false
        survivors = @()
    }
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
        $snapshot = Get-ProcessSnapshot @{} ([int[]]@($startedProcessId))
        if (-not $snapshot.ContainsKey($startedProcessId)) { throw "Started process $startedProcessId was not observable." }
        $seed = $snapshot[$startedProcessId]
        if (-not (Test-SamePath $seed.ImagePath $ExePath)) { throw 'Started process image path did not match SakuraExe.' }
        $owned[$seed.Id] = $seed
        $assigned = [NativeStartupProbe]::AssignProcessToKillOnCloseJob($job, $startedProcessId)
        if (-not $assigned.Succeeded) {
            throw "Could not assign the started process to the run-owned job (Win32 $($assigned.ErrorCode))."
        }
        $jobMembers = @(Get-JobProcessRecords $job $owned)
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
        if (-not [NativeStartupProbe]::CloseNativeHandle($threadHandle)) { throw 'Could not close the suspended process thread handle.' }
        $threadHandle = [IntPtr]::Zero
        if (-not [NativeStartupProbe]::CloseNativeHandle($processHandle)) { throw 'Could not close the suspended process handle.' }
        $processHandle = [IntPtr]::Zero
        $targetCaption = [IO.Path]::GetFileName($DocumentPath)
        $selection = @{ Window = $null }
        $editorIdentified = Wait-WithPoll {
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
        if (-not $editorIdentified) { throw 'Timed out waiting for a run-owned TextEditorWindow.' }
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
                if (-not $readiness.inputIdleReached) {
                    try {
                        # A zero timeout observes this pass only. Keep the Process instance
                        # for the whole loop so polling does not repeatedly open a handle.
                        $inputIdleReady = [bool]$inputProcess.WaitForInputIdle(0)
                    }
                    catch {
                        $result.inputIdleError = $_.Exception.Message
                        throw "WaitForInputIdle failed for the selected editor process: $($result.inputIdleError)"
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
                $result.inputIdleError = if (-not $readiness.inputIdleReached) { 'WaitForInputIdle did not reach idle before the startup timeout.' } else { $null }
                throw ('Timed out waiting for startup milestones: {0}.' -f ($missingMilestones -join '; '))
            }
        }
        finally {
            if ($null -ne $inputProcess) { $inputProcess.Dispose() }
        }
        if ($AffinityMask -ne 0) {
            try {
                $finalSnapshot = Get-ProcessSnapshot $owned
                Update-OwnedProcesses $owned $finalSnapshot
                $currentRecords = @(Get-TrackedOwnedProcesses $owned)
                foreach ($record in @($owned.Values)) {
                    $current = @($currentRecords | Where-Object { [int]$_.Id -eq [int]$record.Id })
                    if ($current.Count -ne 1) {
                        throw "Could not verify the identity of run-owned process $($record.Id) while checking affinity."
                    }
                    [void](Read-ProcessAffinityVerified -ProcessId ([int]$record.Id) -Mask $AffinityMask)
                }
                $result.affinity.descendantsVerified = $true
            }
            catch {
                $result.affinity.descendantsVerified = $false
                $result.affinity.verified = $false
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
        try {
            # Any failure before ResumeSuspendedProcess leaves the process in a
            # suspended state.  Terminate it through the still-owned native
            # handle before closing the job; this cannot target a reused PID.
            if ($processHandle -ne [IntPtr]::Zero) {
                if (-not $processResumed) {
                    $terminated = [NativeStartupProbe]::TerminateProcessHandle($processHandle)
                    if (-not $terminated.Succeeded -and $terminated.Existed) { throw "Could not terminate the unresumed run-owned process (Win32 $($terminated.ErrorCode))." }
                }
                if (-not [NativeStartupProbe]::CloseNativeHandle($processHandle)) { throw 'Could not close the run-owned process handle.' }
                $processHandle = [IntPtr]::Zero
            }
            if ($threadHandle -ne [IntPtr]::Zero) {
                if (-not [NativeStartupProbe]::CloseNativeHandle($threadHandle)) { throw 'Could not close the run-owned thread handle.' }
                $threadHandle = [IntPtr]::Zero
            }
            $cleanup = Stop-OwnedProcesses $owned $job $ExePath
            $job = [IntPtr]::Zero
            $survivors = @($cleanup.survivors)
            $result.survivors = @($survivors | ForEach-Object { [ordered]@{ pid = $_.Id; creation = $_.Creation; imagePath = $_.ImagePath; parentPid = $_.ParentId } })
            $result.jobQuerySucceeded = [bool]$cleanup.jobQuerySucceeded
            $result.jobCloseSucceeded = [bool]$cleanup.jobCloseSucceeded
            $result.finalPathSweepVerified = [bool]$cleanup.finalPathSweepVerified
            $result.processCleanupVerified = $survivors.Count -eq 0 -and [bool]$cleanup.jobQuerySucceeded -and [bool]$cleanup.jobCloseSucceeded -and [bool]$cleanup.finalPathSweepVerified -and [string]::IsNullOrWhiteSpace([string]$cleanup.error)
            if (-not $result.processCleanupVerified) {
                if ([string]::IsNullOrWhiteSpace([string]$cleanup.error)) { throw 'Owned Sakura processes survived or containment verification failed.' }
                throw [string]$cleanup.error
            }
        }
        catch {
            $result.processCleanupVerified = $false
            $result.success = $false
            $cleanupError = "Process cleanup failed: $($_.Exception.Message)"
            if ([string]::IsNullOrEmpty([string]$result.error)) { $result.error = $cleanupError }
            else { $result.error = "$($result.error) $cleanupError" }
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
    [void](Get-ProcessSnapshot @{} ([int[]]@($PID)))
    $snapshotWatch = [Diagnostics.Stopwatch]::StartNew()
    $snapshot = Get-ProcessSnapshot @{} ([int[]]@($PID))
    $snapshotWatch.Stop()
    if (-not $snapshot.ContainsKey($PID)) { throw 'Native process snapshot self-test failed.' }
    if ((Get-Sha256 $PSCommandPath).Length -ne 64) { throw 'SHA-256 self-test failed.' }
    if ([NativeStartupProbe]::GetVerticalScrollMaximum([IntPtr]::Zero) -ne -1) { throw 'Scroll-range self-test failed.' }
    $selfTestTraceRoot = [IO.Path]::GetFullPath((Join-Path $HOME 'tmp'))
    [IO.Directory]::CreateDirectory($selfTestTraceRoot) | Out-Null
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
        workingDirectorySelfTestVerified = $true
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
        generatedAtUtc = [DateTime]::UtcNow.ToString('o'); scriptVersion = '1.13'; runId = $runId; reportPath = $reportPath
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
