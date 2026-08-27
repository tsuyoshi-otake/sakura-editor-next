#requires -Version 5.1
<#!
.SYNOPSIS
  Collects bounded, payload-free MSVC incremental-build evidence for native Rust.

.DESCRIPTION
  The verifier builds only the product project in a detached worktree.  It
  records a baseline, at least three no-op builds, one Rust source mutation,
  one Rust OutputService provider mutation, and one OutputServiceRustProvider.cpp
  mutation.  Build output is classified
  into tool/action kinds, but command lines and source payloads are never
  copied into the evidence JSON.

  The worktree and every generated file live below build/tmp.  The shared
  checkout is never used as the build working tree and only the exact owned
  worktree is removed during cleanup.
#>
[CmdletBinding()]
param(
    [ValidateSet('x64')]
    [string] $Platform = 'x64',
    [ValidateSet('Debug', 'Release')]
    [string] $Configuration = 'Release',
    [ValidateRange(3, 20)]
    [int] $NoOpIterations = 3,
    [ValidateRange(1, 3600)]
    [int] $TimeoutSeconds = 900,
    [string] $WorkspaceRoot = 'build/tmp/nri',
    [string] $Output = 'build/evidence/native-rust-incremental.json',
    [switch] $KeepWorkspace,
    [switch] $SelfTest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:SchemaVersion = 1
$script:VerifierName = 'verify-native-rust-incremental.ps1'
$script:RustBuildTarget = 'BuildSakuraNativeFfi'
$script:WorkActionKinds = @(
    'cargo', 'rustc', 'cl', 'link', 'lib', 'rc', 'mt', 'cmake',
    'senp-tool', 'vcpkg-applocal', 'delete'
)
$script:UnexpectedActionKinds = @('cargo-preflight', 'unexpected_tool')
$script:MaxDiagnosticErrorCodes = 32
$script:MaxRetainedActions = 256
$script:MaxUnexpectedToolNames = 32
$script:MaxProcessFailureRecords = 64
$script:MaxProcessFailureCodes = 32
$script:MaxCleanupInspectionEntries = 1000000
$script:ExpectedLinkConsumers = [ordered]@{
    baseline = @('sakura_core/sakura.vcxproj')
    rust_source = @('sakura_core/sakura.vcxproj')
    rust_output_provider = @('sakura_core/sakura.vcxproj')
    cpp_provider = @('sakura_core/sakura.vcxproj')
}

function New-OrderedObject {
    param([Parameter(Mandatory)][object] $Properties)
    return [pscustomobject]$Properties
}

function Initialize-OwnedProcessInterop {
    if ($null -ne ('Sakura.NativeRustVerifier.OwnedProcess' -as [type])) { return }
    Add-Type -Language CSharp -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using Microsoft.Win32.SafeHandles;

namespace Sakura.NativeRustVerifier
{
    public sealed class OwnedProcess : IDisposable
    {
        private const uint CREATE_SUSPENDED = 0x00000004;
        private const uint EXTENDED_STARTUPINFO_PRESENT = 0x00080000;
        private const uint CREATE_NO_WINDOW = 0x08000000;
        private const uint STARTF_USESTDHANDLES = 0x00000100;
        private const uint HANDLE_FLAG_INHERIT = 0x00000001;
        private const uint PROCESS_QUERY_LIMITED_INFORMATION = 0x00001000;
        private const uint JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x00002000;
        private const uint WAIT_OBJECT_0 = 0x00000000;
        private const uint WAIT_TIMEOUT = 0x00000102;
        private const uint WAIT_FAILED = 0xffffffff;
        private const uint INFINITE = 0xffffffff;
        private const int JobObjectBasicProcessIdList = 3;
        private const int JobObjectExtendedLimitInformation = 9;
        private const int ERROR_MORE_DATA = 234;
        private const int ProcessIdBufferBytes = 65536;
        private static readonly IntPtr PROC_THREAD_ATTRIBUTE_HANDLE_LIST = new IntPtr(0x00020002);
        private static readonly IntPtr PROC_THREAD_ATTRIBUTE_JOB_LIST = new IntPtr(0x0002000D);

        [StructLayout(LayoutKind.Sequential)]
        private struct SECURITY_ATTRIBUTES
        {
            public int nLength;
            public IntPtr lpSecurityDescriptor;
            [MarshalAs(UnmanagedType.Bool)] public bool bInheritHandle;
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
        private struct STARTUPINFOEX
        {
            public STARTUPINFO StartupInfo;
            public IntPtr lpAttributeList;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct PROCESS_INFORMATION
        {
            public IntPtr hProcess;
            public IntPtr hThread;
            public uint dwProcessId;
            public uint dwThreadId;
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
            public UIntPtr JobMemoryLimit;
            public UIntPtr PeakProcessMemoryUsed;
            public UIntPtr PeakJobMemoryUsed;
        }

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern IntPtr CreateJobObject(IntPtr securityAttributes, string name);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool SetInformationJobObject(
            IntPtr job, int informationClass,
            ref JOBOBJECT_EXTENDED_LIMIT_INFORMATION information, uint informationLength);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool TerminateJobObject(IntPtr job, uint exitCode);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool QueryInformationJobObject(
            IntPtr job, int informationClass, IntPtr information,
            uint informationLength, IntPtr returnLength);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool CreatePipe(
            out IntPtr readPipe, out IntPtr writePipe,
            ref SECURITY_ATTRIBUTES pipeAttributes, uint size);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool SetHandleInformation(IntPtr handle, uint mask, uint flags);

        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool CreateProcessW(
            string applicationName, StringBuilder commandLine,
            IntPtr processAttributes, IntPtr threadAttributes,
            [MarshalAs(UnmanagedType.Bool)] bool inheritHandles,
            uint creationFlags, IntPtr environment, string currentDirectory,
            ref STARTUPINFOEX startupInfo, out PROCESS_INFORMATION processInformation);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool InitializeProcThreadAttributeList(
            IntPtr attributeList, int attributeCount, uint flags, ref IntPtr size);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool UpdateProcThreadAttribute(
            IntPtr attributeList, uint flags, IntPtr attribute, IntPtr value,
            IntPtr size, IntPtr previousValue, IntPtr returnSize);

        [DllImport("kernel32.dll")]
        private static extern void DeleteProcThreadAttributeList(IntPtr attributeList);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern IntPtr OpenProcess(
            uint desiredAccess, [MarshalAs(UnmanagedType.Bool)] bool inheritHandle, int processId);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool IsProcessInJob(
            IntPtr process, IntPtr job, [MarshalAs(UnmanagedType.Bool)] out bool result);

        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool QueryFullProcessImageNameW(
            IntPtr process, uint flags, StringBuilder executableName, ref uint size);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern uint ResumeThread(IntPtr thread);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool GetExitCodeProcess(IntPtr process, out uint exitCode);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool CloseHandle(IntPtr handle);

        private IntPtr jobHandle;
        private IntPtr processHandle;
        private FileStream standardOutput;
        private FileStream standardError;
        private int processId;
        private bool disposed;

        private OwnedProcess() { }

        public int Id { get { return processId; } }
        public Stream StandardOutputStream { get { return standardOutput; } }
        public Stream StandardErrorStream { get { return standardError; } }

        public bool HasExited
        {
            get
            {
                EnsureNotDisposed();
                uint result = WaitForSingleObject(processHandle, 0);
                if (result == WAIT_FAILED) ThrowLastError("WaitForSingleObject");
                return result == WAIT_OBJECT_0;
            }
        }

        public int ExitCode
        {
            get
            {
                EnsureNotDisposed();
                uint exitCode;
                if (!GetExitCodeProcess(processHandle, out exitCode)) ThrowLastError("GetExitCodeProcess");
                return unchecked((int)exitCode);
            }
        }

        public static OwnedProcess Start(string applicationName, string commandLine, string workingDirectory)
        {
            if (String.IsNullOrWhiteSpace(applicationName)) throw new ArgumentException("applicationName");
            if (String.IsNullOrWhiteSpace(commandLine) || commandLine.Length >= 32767) throw new ArgumentException("commandLine");
            if (commandLine.IndexOf('\r') >= 0 || commandLine.IndexOf('\n') >= 0) throw new ArgumentException("commandLine");
            OwnedProcess owned = new OwnedProcess();
            IntPtr stdoutRead = IntPtr.Zero;
            IntPtr stdoutWrite = IntPtr.Zero;
            IntPtr stderrRead = IntPtr.Zero;
            IntPtr stderrWrite = IntPtr.Zero;
            IntPtr stdinRead = IntPtr.Zero;
            IntPtr stdinWrite = IntPtr.Zero;
            IntPtr attributeList = IntPtr.Zero;
            IntPtr handleList = IntPtr.Zero;
            IntPtr jobList = IntPtr.Zero;
            bool attributeListInitialized = false;
            PROCESS_INFORMATION processInformation = new PROCESS_INFORMATION();
            try
            {
                owned.jobHandle = CreateJobObject(IntPtr.Zero, null);
                if (owned.jobHandle == IntPtr.Zero) ThrowLastError("CreateJobObject");
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = new JOBOBJECT_EXTENDED_LIMIT_INFORMATION();
                limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                if (!SetInformationJobObject(
                    owned.jobHandle, JobObjectExtendedLimitInformation, ref limits,
                    (uint)Marshal.SizeOf(typeof(JOBOBJECT_EXTENDED_LIMIT_INFORMATION))))
                {
                    ThrowLastError("SetInformationJobObject");
                }

                SECURITY_ATTRIBUTES pipeAttributes = new SECURITY_ATTRIBUTES();
                pipeAttributes.nLength = Marshal.SizeOf(typeof(SECURITY_ATTRIBUTES));
                pipeAttributes.bInheritHandle = true;
                if (!CreatePipe(out stdoutRead, out stdoutWrite, ref pipeAttributes, 0)) ThrowLastError("CreatePipe stdout");
                if (!SetHandleInformation(stdoutRead, HANDLE_FLAG_INHERIT, 0)) ThrowLastError("SetHandleInformation stdout");
                if (!CreatePipe(out stderrRead, out stderrWrite, ref pipeAttributes, 0)) ThrowLastError("CreatePipe stderr");
                if (!SetHandleInformation(stderrRead, HANDLE_FLAG_INHERIT, 0)) ThrowLastError("SetHandleInformation stderr");
                if (!CreatePipe(out stdinRead, out stdinWrite, ref pipeAttributes, 0)) ThrowLastError("CreatePipe stdin");
                if (!SetHandleInformation(stdinWrite, HANDLE_FLAG_INHERIT, 0)) ThrowLastError("SetHandleInformation stdin");

                IntPtr attributeListSize = IntPtr.Zero;
                InitializeProcThreadAttributeList(IntPtr.Zero, 2, 0, ref attributeListSize);
                if (attributeListSize == IntPtr.Zero) ThrowLastError("InitializeProcThreadAttributeList size");
                attributeList = Marshal.AllocHGlobal(attributeListSize);
                if (!InitializeProcThreadAttributeList(attributeList, 2, 0, ref attributeListSize))
                {
                    ThrowLastError("InitializeProcThreadAttributeList");
                }
                attributeListInitialized = true;

                handleList = Marshal.AllocHGlobal(IntPtr.Size * 3);
                Marshal.WriteIntPtr(handleList, 0, stdinRead);
                Marshal.WriteIntPtr(handleList, IntPtr.Size, stdoutWrite);
                Marshal.WriteIntPtr(handleList, IntPtr.Size * 2, stderrWrite);
                if (!UpdateProcThreadAttribute(
                    attributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handleList,
                    new IntPtr(IntPtr.Size * 3), IntPtr.Zero, IntPtr.Zero))
                {
                    ThrowLastError("UpdateProcThreadAttribute handle list");
                }

                jobList = Marshal.AllocHGlobal(IntPtr.Size);
                Marshal.WriteIntPtr(jobList, owned.jobHandle);
                if (!UpdateProcThreadAttribute(
                    attributeList, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, jobList,
                    new IntPtr(IntPtr.Size), IntPtr.Zero, IntPtr.Zero))
                {
                    ThrowLastError("UpdateProcThreadAttribute job list");
                }

                STARTUPINFOEX startupInfo = new STARTUPINFOEX();
                startupInfo.StartupInfo.cb = Marshal.SizeOf(typeof(STARTUPINFOEX));
                startupInfo.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
                startupInfo.StartupInfo.hStdInput = stdinRead;
                startupInfo.StartupInfo.hStdOutput = stdoutWrite;
                startupInfo.StartupInfo.hStdError = stderrWrite;
                startupInfo.lpAttributeList = attributeList;
                if (!CreateProcessW(
                    applicationName, new StringBuilder(commandLine), IntPtr.Zero, IntPtr.Zero, true,
                    CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                    IntPtr.Zero, workingDirectory,
                    ref startupInfo, out processInformation))
                {
                    ThrowLastError("CreateProcessW");
                }
                owned.processHandle = processInformation.hProcess;
                owned.processId = unchecked((int)processInformation.dwProcessId);

                CloseRequired(ref stdoutWrite);
                CloseRequired(ref stderrWrite);
                CloseRequired(ref stdinRead);
                CloseRequired(ref stdinWrite);
                owned.standardOutput = AttachReadablePipe(ref stdoutRead);
                owned.standardError = AttachReadablePipe(ref stderrRead);
                uint resumeResult = ResumeThread(processInformation.hThread);
                if (resumeResult == UInt32.MaxValue) ThrowLastError("ResumeThread");
                CloseRequired(ref processInformation.hThread);
                return owned;
            }
            catch (Exception startFailure)
            {
                // A successfully created process is already in owned.jobHandle
                // because PROC_THREAD_ATTRIBUTE_JOB_LIST is applied atomically
                // by CreateProcessW. Closing that kill-on-close Job is therefore
                // sufficient even when setup fails before ResumeThread.
                Exception cleanupFailure = null;
                try
                {
                    owned.DisposeVerified();
                }
                catch (Exception error)
                {
                    cleanupFailure = error;
                    owned.DisposeBestEffort();
                }
                bool rawHandlesClosed =
                    CloseBestEffortValue(processInformation.hThread) &
                    CloseBestEffortValue(stdoutRead) &
                    CloseBestEffortValue(stdoutWrite) &
                    CloseBestEffortValue(stderrRead) &
                    CloseBestEffortValue(stderrWrite) &
                    CloseBestEffortValue(stdinRead) &
                    CloseBestEffortValue(stdinWrite);
                if (cleanupFailure != null || !rawHandlesClosed)
                {
                    throw new InvalidOperationException(
                        "owned process startup cleanup could not be verified", cleanupFailure ?? startFailure);
                }
                throw;
            }
            finally
            {
                if (attributeListInitialized) DeleteProcThreadAttributeList(attributeList);
                if (jobList != IntPtr.Zero) Marshal.FreeHGlobal(jobList);
                if (handleList != IntPtr.Zero) Marshal.FreeHGlobal(handleList);
                if (attributeList != IntPtr.Zero) Marshal.FreeHGlobal(attributeList);
            }
        }

        public bool WaitForExit(int milliseconds)
        {
            EnsureNotDisposed();
            uint timeout = milliseconds < 0 ? INFINITE : unchecked((uint)milliseconds);
            uint result = WaitForSingleObject(processHandle, timeout);
            if (result == WAIT_FAILED) ThrowLastError("WaitForSingleObject");
            if (result != WAIT_OBJECT_0 && result != WAIT_TIMEOUT) throw new InvalidOperationException("unexpected process wait result");
            return result == WAIT_OBJECT_0;
        }

        public void WaitForExit()
        {
            WaitForExit(-1);
        }

        public int[] GetActiveProcessIds()
        {
            EnsureNotDisposed();
            IntPtr buffer = Marshal.AllocHGlobal(ProcessIdBufferBytes);
            try
            {
                if (!QueryInformationJobObject(
                    jobHandle, JobObjectBasicProcessIdList, buffer,
                    ProcessIdBufferBytes, IntPtr.Zero))
                {
                    int error = Marshal.GetLastWin32Error();
                    if (error == ERROR_MORE_DATA) throw new InvalidOperationException("job process list exceeded its bounded capacity");
                    throw new Win32Exception(error, "QueryInformationJobObject");
                }
                uint count = unchecked((uint)Marshal.ReadInt32(buffer, 4));
                int capacity = (ProcessIdBufferBytes - 8) / IntPtr.Size;
                if (count > capacity) throw new InvalidOperationException("job process list count exceeded its bounded capacity");
                List<int> result = new List<int>((int)count);
                for (int index = 0; index < count; index++)
                {
                    long value = IntPtr.Size == 8
                        ? Marshal.ReadInt64(buffer, 8 + (index * IntPtr.Size))
                        : Marshal.ReadInt32(buffer, 8 + (index * IntPtr.Size));
                    result.Add(unchecked((int)value));
                }
                return result.ToArray();
            }
            finally
            {
                Marshal.FreeHGlobal(buffer);
            }
        }

        public string[] GetActiveProcessNames()
        {
            EnsureNotDisposed();
            int[] processIds = GetActiveProcessIds();
            List<string> result = new List<string>(processIds.Length);
            foreach (int activeProcessId in processIds)
            {
                IntPtr rawProcess = OpenProcess(
                    PROCESS_QUERY_LIMITED_INFORMATION, false, activeProcessId);
                if (rawProcess == IntPtr.Zero)
                {
                    // The process may have exited between the Job query and
                    // OpenProcess. Retaining an unknown record is fail-closed:
                    // it can never be accepted as an allowed compiler helper.
                    result.Add("unknown");
                    continue;
                }
                SafeFileHandle activeProcess = new SafeFileHandle(rawProcess, true);
                try
                {
                    bool isMember;
                    if (!IsProcessInJob(activeProcess.DangerousGetHandle(), jobHandle, out isMember))
                    {
                        ThrowLastError("IsProcessInJob");
                    }
                    if (!isMember)
                    {
                        result.Add("unknown");
                        continue;
                    }
                    StringBuilder executableName = new StringBuilder(32768);
                    uint size = unchecked((uint)executableName.Capacity);
                    if (!QueryFullProcessImageNameW(
                        activeProcess.DangerousGetHandle(), 0, executableName, ref size))
                    {
                        result.Add("unknown");
                        continue;
                    }
                    string name = Path.GetFileName(executableName.ToString());
                    result.Add(String.IsNullOrWhiteSpace(name) ? "unknown" : name);
                }
                finally
                {
                    CloseSafeHandleVerified(activeProcess);
                }
            }
            return result.ToArray();
        }

        public void Kill()
        {
            EnsureNotDisposed();
            if (!TerminateJobObject(jobHandle, 1)) ThrowLastError("TerminateJobObject");
        }

        public bool WaitForJobEmpty(int milliseconds)
        {
            Stopwatch stopwatch = Stopwatch.StartNew();
            do
            {
                if (GetActiveProcessIds().Length == 0) return true;
                Thread.Sleep(25);
            }
            while (stopwatch.ElapsedMilliseconds < milliseconds);
            return GetActiveProcessIds().Length == 0;
        }

        public bool WaitForProcessRemoval(int targetProcessId, int milliseconds)
        {
            Stopwatch stopwatch = Stopwatch.StartNew();
            do
            {
                if (Array.IndexOf(GetActiveProcessIds(), targetProcessId) < 0) return true;
                Thread.Sleep(25);
            }
            while (stopwatch.ElapsedMilliseconds < milliseconds);
            return Array.IndexOf(GetActiveProcessIds(), targetProcessId) < 0;
        }

        public void DisposeVerified()
        {
            if (disposed) return;
            Exception failure = null;
            try
            {
                if (jobHandle != IntPtr.Zero && GetActiveProcessIds().Length != 0)
                {
                    if (!TerminateJobObject(jobHandle, 1)) ThrowLastError("TerminateJobObject during dispose");
                    if (!WaitForJobEmpty(5000)) throw new InvalidOperationException("Job Object was not empty during dispose");
                }
            }
            catch (Exception error)
            {
                failure = error;
                if (jobHandle != IntPtr.Zero) TerminateJobObject(jobHandle, 1);
            }
            if (standardOutput != null)
            {
                try { standardOutput.Dispose(); standardOutput = null; }
                catch (Exception error) { if (failure == null) failure = error; }
            }
            if (standardError != null)
            {
                try { standardError.Dispose(); standardError = null; }
                catch (Exception error) { if (failure == null) failure = error; }
            }
            try { CloseRequired(ref jobHandle); }
            catch (Exception error) { if (failure == null) failure = error; }
            try { CloseRequired(ref processHandle); }
            catch (Exception error) { if (failure == null) failure = error; }
            bool complete = standardOutput == null && standardError == null &&
                jobHandle == IntPtr.Zero && processHandle == IntPtr.Zero;
            if (failure != null || !complete)
            {
                throw new InvalidOperationException("owned process cleanup could not be verified", failure);
            }
            disposed = true;
            GC.SuppressFinalize(this);
        }

        public void Dispose()
        {
            DisposeVerified();
        }

        ~OwnedProcess()
        {
            DisposeBestEffort();
        }

        private void EnsureNotDisposed()
        {
            if (disposed) throw new ObjectDisposedException("OwnedProcess");
        }

        private static void CloseRequired(ref IntPtr handle)
        {
            if (handle == IntPtr.Zero) return;
            if (!CloseHandle(handle)) ThrowLastError("CloseHandle");
            handle = IntPtr.Zero;
        }

        private static void CloseBestEffort(ref IntPtr handle)
        {
            if (handle == IntPtr.Zero) return;
            if (CloseHandle(handle)) handle = IntPtr.Zero;
        }

        private static bool CloseBestEffortValue(IntPtr handle)
        {
            return handle == IntPtr.Zero || CloseHandle(handle);
        }

        private static FileStream AttachReadablePipe(ref IntPtr readHandle)
        {
            if (readHandle == IntPtr.Zero) throw new ArgumentException("readHandle");
            // Construct the SafeHandle while the raw variable still owns the
            // handle. Ownership moves before FileStream construction, so a
            // FileStream constructor exception has one and only one closer.
            SafeFileHandle safeHandle = new SafeFileHandle(readHandle, true);
            readHandle = IntPtr.Zero;
            try
            {
                return new FileStream(safeHandle, FileAccess.Read, 4096, false);
            }
            catch
            {
                safeHandle.Dispose();
                throw;
            }
        }

        private static void CloseSafeHandleVerified(SafeHandle handle)
        {
            if (handle == null || handle.IsClosed || handle.IsInvalid) return;
            bool addedReference = false;
            try
            {
                handle.DangerousAddRef(ref addedReference);
                if (!CloseHandle(handle.DangerousGetHandle())) ThrowLastError("CloseHandle");
                handle.SetHandleAsInvalid();
            }
            finally
            {
                if (addedReference) handle.DangerousRelease();
                handle.Dispose();
            }
        }

        private void DisposeBestEffort()
        {
            if (disposed) return;
            if (jobHandle != IntPtr.Zero)
            {
                TerminateJobObject(jobHandle, 1);
            }
            try { if (standardOutput != null) { standardOutput.Dispose(); standardOutput = null; } } catch { }
            try { if (standardError != null) { standardError.Dispose(); standardError = null; } } catch { }
            CloseBestEffort(ref jobHandle);
            CloseBestEffort(ref processHandle);
            disposed = standardOutput == null && standardError == null &&
                jobHandle == IntPtr.Zero && processHandle == IntPtr.Zero;
            if (disposed) GC.SuppressFinalize(this);
        }

        private static void ThrowLastError(string operation)
        {
            throw new Win32Exception(Marshal.GetLastWin32Error(), operation);
        }
    }
}
'@
}

function Get-RepositoryRoot {
    $candidate = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
    if (-not [IO.Directory]::Exists($candidate)) {
        throw "repository root does not exist: $candidate"
    }
    if (-not ([IO.File]::Exists((Join-Path $candidate '.git')) -or
              [IO.Directory]::Exists((Join-Path $candidate '.git')))) {
        throw "repository root is not a Git checkout: $candidate"
    }
    return (Resolve-Path -LiteralPath $candidate).Path
}

function Get-FullPath {
    param([Parameter(Mandatory)][string] $Path)
    return [IO.Path]::GetFullPath($Path)
}

function Test-PathBelow {
    param(
        [Parameter(Mandatory)][string] $Path,
        [Parameter(Mandatory)][string] $Root,
        [switch] $AllowRoot
    )
    $fullPath = Get-FullPath $Path
    $fullRoot = (Get-FullPath $Root).TrimEnd([char[]]'\/')
    if ($AllowRoot -and $fullPath.Equals($fullRoot, [StringComparison]::OrdinalIgnoreCase)) {
        return $true
    }
    return $fullPath.StartsWith($fullRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) -or
        $fullPath.StartsWith($fullRoot + [IO.Path]::AltDirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)
}

function Assert-PathBelow {
    param(
        [Parameter(Mandatory)][string] $Path,
        [Parameter(Mandatory)][string] $Root,
        [Parameter(Mandatory)][string] $Context,
        [switch] $AllowRoot
    )
    if (-not (Test-PathBelow -Path $Path -Root $Root -AllowRoot:$AllowRoot)) {
        throw "$Context must remain below ${Root}: $Path"
    }
    return (Get-FullPath $Path)
}

function Get-RelativePath {
    param(
        [Parameter(Mandatory)][string] $Path,
        [Parameter(Mandatory)][string] $Root
    )
    $fullPath = Get-FullPath $Path
    $fullRoot = (Get-FullPath $Root).TrimEnd([char[]]'\/')
    if ($fullPath.Equals($fullRoot, [StringComparison]::OrdinalIgnoreCase)) {
        return ''
    }
    if (-not (Test-PathBelow -Path $fullPath -Root $fullRoot)) {
        return '<outside-workspace>'
    }
    return $fullPath.Substring($fullRoot.Length).TrimStart([char[]]'\/').Replace('\', '/')
}

function Assert-NoReparsePoint {
    param([Parameter(Mandatory)][string] $Path, [Parameter(Mandatory)][string] $Context)
    $fullPath = Get-FullPath $Path
    $current = $fullPath
    while ($true) {
        if ([IO.Directory]::Exists($current) -or [IO.File]::Exists($current)) {
            $item = Get-Item -LiteralPath $current -Force
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "$Context contains a reparse point: $current"
            }
        }
        $parent = Split-Path -Parent $current
        if ([string]::IsNullOrEmpty($parent) -or $parent.Equals($current, [StringComparison]::OrdinalIgnoreCase)) {
            break
        }
        $current = $parent
    }
}

function Assert-NoDirectoryReparsePointsBelow {
    param([Parameter(Mandatory)][string] $Path, [Parameter(Mandatory)][string] $Context)
    $fullPath = Get-FullPath $Path
    if (-not ([IO.Directory]::Exists($fullPath) -or [IO.File]::Exists($fullPath))) { return }
    Assert-NoReparsePoint -Path $fullPath -Context $Context
    $root = Get-Item -LiteralPath $fullPath -Force
    if ($root -isnot [IO.DirectoryInfo]) { return }

    # Enumerate one level at a time with a stack. This is bounded and avoids
    # provider recursion, and every entry is checked before it can be pushed.
    $pending = [System.Collections.Generic.Stack[string]]::new()
    $pending.Push($fullPath)
    $inspected = 0
    while ($pending.Count -gt 0) {
        $current = $pending.Pop()
        foreach ($entry in [IO.Directory]::EnumerateFileSystemEntries($current, '*', [IO.SearchOption]::TopDirectoryOnly)) {
            $inspected++
            if ($inspected -gt $script:MaxCleanupInspectionEntries) {
                throw "$Context inspection exceeded its bounded entry limit"
            }
            $attributes = [IO.File]::GetAttributes($entry)
            $isReparsePoint = ($attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0
            $isDirectory = ($attributes -band [IO.FileAttributes]::Directory) -ne 0
            if ($isReparsePoint -and $isDirectory) {
                throw "$Context contains a reparse point: $entry"
            }
            if ($isDirectory) {
                $pending.Push($entry)
            }
        }
    }
}

function Get-Sha256 {
    param([Parameter(Mandatory)][string] $Path)
    # Windows PowerShell 5.1 does not guarantee the Get-FileHash cmdlet on the
    # host image.  Use the framework API so both supported PowerShell hosts
    # hash the same exact bytes without depending on an optional module.
    $stream = [IO.File]::OpenRead($Path)
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try {
        return ([BitConverter]::ToString($algorithm.ComputeHash($stream))).Replace('-', '').ToLowerInvariant()
    } finally {
        $algorithm.Dispose()
        $stream.Dispose()
    }
}

function Get-StringSequenceSha256 {
    param([AllowEmptyCollection()][string[]] $Lines = @())
    $text = @($Lines) -join "`n"
    $bytes = (New-Object Text.UTF8Encoding($false)).GetBytes($text)
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try {
        return ([BitConverter]::ToString($algorithm.ComputeHash($bytes))).Replace('-', '').ToLowerInvariant()
    } finally {
        $algorithm.Dispose()
    }
}

function Get-SanitizedProcessName {
    param([AllowNull()][string] $Name)
    if ([string]::IsNullOrWhiteSpace($Name)) { return 'unknown' }
    try { $leaf = [IO.Path]::GetFileName($Name) } catch { $leaf = $Name }
    if ([string]::IsNullOrWhiteSpace($leaf)) { return 'unknown' }
    if ($leaf -notmatch '^(?i)[A-Za-z0-9][A-Za-z0-9_.-]{0,63}$') { return 'unknown' }
    return $leaf.ToLowerInvariant()
}

function Get-ProcessNameCounts {
    param(
        [AllowNull()][AllowEmptyCollection()][object[]] $Identities = @()
    )
    $counts = @{}
    foreach ($identity in $Identities) {
        $name = Get-SanitizedProcessName ([string]$identity.name)
        if ($counts.ContainsKey($name)) { $counts[$name] = [int]$counts[$name] + 1 }
        else { $counts[$name] = 1 }
    }
    $ordered = [ordered]@{}
    foreach ($name in @($counts.Keys | Sort-Object)) { $ordered[$name] = [int]$counts[$name] }
    return (New-OrderedObject $ordered)
}

function Get-OwnedJobMemberRecords {
    param([Parameter(Mandatory)][object] $OwnedProcess)
    $processNames = @($OwnedProcess.GetActiveProcessNames())
    if ($processNames.Count -eq 0) { return @() }
    $records = [System.Collections.Generic.List[object]]::new()
    foreach ($processName in $processNames) {
        [void]$records.Add((New-OrderedObject ([ordered]@{
            name = Get-SanitizedProcessName ([string]$processName)
        })))
    }
    return $records.ToArray()
}

function Get-OwnedJobMemberDisposition {
    param(
        [Parameter(Mandatory)][AllowEmptyCollection()][object[]] $Records,
        [AllowNull()][AllowEmptyCollection()][string[]] $AllowedHelperNames = @()
    )
    $allowedNames = @{}
    foreach ($name in $AllowedHelperNames) {
        $sanitized = Get-SanitizedProcessName $name
        if ($sanitized -ne 'unknown') { $allowedNames[$sanitized] = $true }
    }
    $expected = [System.Collections.Generic.List[object]]::new()
    $unexpected = [System.Collections.Generic.List[object]]::new()
    foreach ($record in $Records) {
        $name = Get-SanitizedProcessName ([string]$record.name)
        if ($allowedNames.ContainsKey($name)) { [void]$expected.Add($record) }
        else { [void]$unexpected.Add($record) }
    }
    return (New-OrderedObject ([ordered]@{
        expected = $expected.ToArray()
        unexpected = $unexpected.ToArray()
    }))
}

function ConvertTo-WindowsCommandLineArgument {
    param([AllowNull()][AllowEmptyString()][string] $Argument)
    if ($null -eq $Argument) { $Argument = '' }
    # ProcessStartInfo.ArgumentList accepts raw values.  Only the legacy
    # ProcessStartInfo.Arguments fallback needs the CommandLineToArgvW/CRT
    # quoting rules below: double trailing backslashes before the closing
    # quote, and double backslashes before an embedded quote.
    if ($Argument.Length -gt 0 -and $Argument -notmatch '[\s"]') { return $Argument }
    $builder = New-Object Text.StringBuilder
    [void]$builder.Append([char]34)
    $backslashCount = 0
    foreach ($character in $Argument.ToCharArray()) {
        if ($character -eq [char]92) {
            $backslashCount++
            continue
        }
        if ($character -eq [char]34) {
            for ($index = 0; $index -lt (($backslashCount * 2) + 1); $index++) { [void]$builder.Append([char]92) }
            [void]$builder.Append([char]34)
            $backslashCount = 0
            continue
        }
        for ($index = 0; $index -lt $backslashCount; $index++) { [void]$builder.Append([char]92) }
        [void]$builder.Append($character)
        $backslashCount = 0
    }
    for ($index = 0; $index -lt ($backslashCount * 2); $index++) { [void]$builder.Append([char]92) }
    [void]$builder.Append([char]34)
    return $builder.ToString()
}

function ConvertTo-WindowsCommandLine {
    param([AllowNull()][AllowEmptyCollection()][string[]] $Arguments = @())
    return (@($Arguments | ForEach-Object { ConvertTo-WindowsCommandLineArgument -Argument $_ }) -join ' ')
}

function Get-DiagnosticLogSummary {
    param([Parameter(Mandatory)][string] $LogPath)
    if (-not [IO.File]::Exists($LogPath)) {
        return (New-OrderedObject ([ordered]@{
            available = $false
            byteCount = $null
            lineCount = $null
            sha256 = $null
            errorCodes = (New-OrderedObject ([ordered]@{}))
            errorCodesTruncated = $false
        }))
    }
    $item = Get-Item -LiteralPath $LogPath -Force
    if ($item -isnot [IO.FileInfo]) { throw "diagnostic log is not a regular file: $LogPath" }
    $errorPattern = '(?i)(?<![A-Za-z0-9])(?<code>(?:MSB|LNK|RC|MT|C|E)\d{3,5})(?![A-Za-z0-9])'
    $errorCounts = @{}
    $errorCodesTruncated = $false
    $lineCount = 0
    $encoding = [Text.UTF8Encoding]::new($false, $false)
    $reader = New-Object IO.StreamReader($LogPath, $encoding, $true)
    try {
        while (($line = $reader.ReadLine()) -ne $null) {
            $lineCount++
            foreach ($match in [regex]::Matches($line, $errorPattern)) {
                $code = $match.Groups['code'].Value.ToUpperInvariant()
                if (-not $errorCounts.ContainsKey($code)) {
                    if ($errorCounts.Count -ge $script:MaxDiagnosticErrorCodes) {
                        $errorCodesTruncated = $true
                        continue
                    }
                    $errorCounts[$code] = 0
                }
                if ($errorCounts.ContainsKey($code)) { $errorCounts[$code] = [int]$errorCounts[$code] + 1 }
            }
        }
    } finally {
        $reader.Dispose()
    }
    $orderedCodes = [ordered]@{}
    foreach ($code in @($errorCounts.Keys | Sort-Object)) { $orderedCodes[$code] = [int]$errorCounts[$code] }
    return (New-OrderedObject ([ordered]@{
        available = $true
        byteCount = [UInt64]$item.Length
        lineCount = [int]$lineCount
        sha256 = Get-Sha256 $LogPath
        errorCodes = (New-OrderedObject $orderedCodes)
        errorCodesTruncated = [bool]$errorCodesTruncated
    }))
}

function New-EmptyProcessOutputStreamMetadata {
    param([bool] $ParserFailed = $false)
    return (New-OrderedObject ([ordered]@{
        available = $false
        byteCount = $null
        lineCount = $null
        sha256 = $null
        parserFailed = [bool]$ParserFailed
    }))
}

function New-ProcessOutputMetadataFailure {
    return (New-OrderedObject ([ordered]@{
        stdout = New-EmptyProcessOutputStreamMetadata -ParserFailed:$true
        stderr = New-EmptyProcessOutputStreamMetadata -ParserFailed:$true
        outputAvailable = $false
        buildErrors = New-OrderedObject ([ordered]@{})
        errorCodes = New-OrderedObject ([ordered]@{})
        buildErrorRecordCount = 0
        errorCodeRecordCount = 0
        failureRecordCount = 0
        buildErrorsTruncated = $true
        errorCodesTruncated = $true
        failureRecordsTruncated = $true
        parserFailed = $true
        versionMatched = $false
        versionProofAvailable = $false
    }))
}

function Add-BoundedProcessFailureCode {
    param(
        [Parameter(Mandatory)][hashtable] $Counts,
        [Parameter(Mandatory)][string] $Code,
        [Parameter(Mandatory)][hashtable] $State
    )
    $normalized = $Code.ToUpperInvariant()
    if ($normalized -notmatch '^[A-Z][A-Z0-9_]{2,62}$') { return }
    if ([int]$State.recordCount -ge $script:MaxProcessFailureRecords) {
        $State.recordsTruncated = $true
        return
    }
    $State.recordCount = [int]$State.recordCount + 1
    if (-not $Counts.ContainsKey($normalized)) {
        if ($Counts.Count -ge $script:MaxProcessFailureCodes) {
            $State.codesTruncated = $true
            return
        }
        $Counts[$normalized] = 0
    }
    if ($Counts.ContainsKey($normalized)) { $Counts[$normalized] = [int]$Counts[$normalized] + 1 }
}

function Add-ProcessFailureTokens {
    param(
        [Parameter(Mandatory)][AllowEmptyString()][string] $Line,
        [Parameter(Mandatory)][hashtable] $BuildErrorCounts,
        [Parameter(Mandatory)][hashtable] $ErrorCodeCounts,
        [Parameter(Mandatory)][hashtable] $BuildErrorState,
        [Parameter(Mandatory)][hashtable] $ErrorCodeState,
        [Parameter(Mandatory)][hashtable] $OverallState
    )
    # BuildError codes are symbolic and intentionally reduced to a safe token;
    # no exception text, paths, or command payload enters the evidence.
    $buildPattern = '(?i)(?<![A-Za-z0-9_])(?<code>(?:TOOL|PACKAGE|VCPKG|CARGO|MSBUILD|NATIVE|RUST|BUILD)_[A-Z0-9][A-Z0-9_]{1,62})(?![A-Za-z0-9_])'
    foreach ($match in [regex]::Matches($Line, $buildPattern)) {
        Add-BoundedProcessFailureCode -Counts $BuildErrorCounts -Code $match.Groups['code'].Value -State $BuildErrorState
    }
    $namedBuildPattern = '(?i)\bBuildError\b[^A-Za-z0-9_]*(?<code>[A-Z][A-Z0-9_]{2,62})'
    foreach ($match in [regex]::Matches($Line, $namedBuildPattern)) {
        $code = $match.Groups['code'].Value
        if ($code -notmatch '^(?i)(?:TOOL|PACKAGE|VCPKG|CARGO|MSBUILD|NATIVE|RUST|BUILD)_') {
            Add-BoundedProcessFailureCode -Counts $BuildErrorCounts -Code $code -State $BuildErrorState
        }
    }
    $errorPattern = '(?i)(?<![A-Za-z0-9])(?<code>(?:MSB|LNK|RC|MT|C|E)\d{3,5})(?![A-Za-z0-9])'
    foreach ($match in [regex]::Matches($Line, $errorPattern)) {
        Add-BoundedProcessFailureCode -Counts $ErrorCodeCounts -Code $match.Groups['code'].Value -State $ErrorCodeState
    }
    if ($BuildErrorState.recordsTruncated -or $BuildErrorState.codesTruncated -or
        $ErrorCodeState.recordsTruncated -or $ErrorCodeState.codesTruncated) { $OverallState.truncated = $true }
}

function Get-ProcessOutputMetadata {
    param(
        [Parameter(Mandatory)][string] $StdOutPath,
        [Parameter(Mandatory)][string] $StdErrPath,
        [AllowNull()][string] $ExpectedVersionTag
    )
    $buildErrorCounts = @{}
    $errorCodeCounts = @{}
    $buildErrorState = @{ recordCount = 0; recordsTruncated = $false; codesTruncated = $false }
    $errorCodeState = @{ recordCount = 0; recordsTruncated = $false; codesTruncated = $false }
    $overallState = @{ truncated = $false }
    $parserFailed = $false
    $versionMatched = $false
    $streamMetadata = [ordered]@{}
    $streamPaths = [ordered]@{ stdout = $StdOutPath; stderr = $StdErrPath }
    foreach ($streamName in @('stdout', 'stderr')) {
        $path = [string]$streamPaths[$streamName]
        $available = [IO.File]::Exists($path)
        $pathExists = $available -or [IO.Directory]::Exists($path)
        $byteCount = $null
        $lineCount = $null
        $sha256 = $null
        $streamParserFailed = $false
        $reader = $null
        $fileStream = $null
        $algorithm = $null
        try {
            if (-not $pathExists) {
                $streamMetadata[$streamName] = New-EmptyProcessOutputStreamMetadata
                continue
            }
            if (-not $available) { throw 'process output path is not a regular file' }
            $item = Get-Item -LiteralPath $path -Force
            if ($item -isnot [IO.FileInfo]) { throw 'process output is not a regular file' }
            $byteCount = [UInt64]$item.Length
            $lineCount = [UInt64]0
            # The completed async capture must be immutable while evidence is
            # collected. FileShare.Read permits concurrent readers but denies
            # writers and deletion after this handle is acquired. If another
            # handle prevents that snapshot open, the catch below fails closed.
            $share = [IO.FileShare]::Read
            $fileStream = [IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::Read, $share)
            if ([UInt64]$fileStream.Length -ne $byteCount) { throw 'process output length changed before hashing' }
            $algorithm = [Security.Cryptography.SHA256]::Create()
            $sha256 = ([BitConverter]::ToString($algorithm.ComputeHash($fileStream))).Replace('-', '').ToLowerInvariant()
            $algorithm.Dispose()
            $algorithm = $null
            $fileStream.Position = 0
            $reader = [IO.StreamReader]::new($fileStream, [Text.UTF8Encoding]::new($false, $false), $true, 4096, $true)
            while (($line = $reader.ReadLine()) -ne $null) {
                $lineCount++
                if (-not [string]::IsNullOrWhiteSpace($ExpectedVersionTag) -and
                    $line.IndexOf($ExpectedVersionTag, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
                    $versionMatched = $true
                }
                Add-ProcessFailureTokens -Line $line -BuildErrorCounts $buildErrorCounts -ErrorCodeCounts $errorCodeCounts `
                    -BuildErrorState $buildErrorState -ErrorCodeState $errorCodeState -OverallState $overallState
            }
            $reader.Dispose()
            $reader = $null
            if ([UInt64]$fileStream.Length -ne $byteCount) { throw 'process output length changed during parsing' }
            $fileStream.Position = 0
            $algorithm = [Security.Cryptography.SHA256]::Create()
            $sha256After = ([BitConverter]::ToString($algorithm.ComputeHash($fileStream))).Replace('-', '').ToLowerInvariant()
            $algorithm.Dispose()
            $algorithm = $null
            if ($sha256After -ne $sha256) { throw 'process output changed during parsing' }
            $fileStream.Dispose()
            $fileStream = $null
            $after = Get-Item -LiteralPath $path -Force
            if ($after -isnot [IO.FileInfo] -or [UInt64]$after.Length -ne $byteCount) {
                throw 'process output length changed after hashing'
            }
        } catch {
            $streamParserFailed = $true
            $parserFailed = $true
        } finally {
            if ($null -ne $reader) { try { $reader.Dispose() } catch { } }
            if ($null -ne $algorithm) { try { $algorithm.Dispose() } catch { } }
            if ($null -ne $fileStream) { try { $fileStream.Dispose() } catch { } }
        }
        $streamMetadata[$streamName] = New-OrderedObject ([ordered]@{
            available = [bool]$available
            byteCount = $byteCount
            lineCount = $lineCount
            sha256 = $sha256
            parserFailed = [bool]$streamParserFailed
        })
    }
    $orderedBuildErrors = [ordered]@{}
    foreach ($code in @($buildErrorCounts.Keys | Sort-Object)) { $orderedBuildErrors[$code] = [int]$buildErrorCounts[$code] }
    $orderedErrorCodes = [ordered]@{}
    foreach ($code in @($errorCodeCounts.Keys | Sort-Object)) { $orderedErrorCodes[$code] = [int]$errorCodeCounts[$code] }
    $outputAvailable = [bool]$streamMetadata.stdout.available -or [bool]$streamMetadata.stderr.available
    return (New-OrderedObject ([ordered]@{
        stdout = $streamMetadata.stdout
        stderr = $streamMetadata.stderr
        outputAvailable = $outputAvailable
        buildErrors = New-OrderedObject $orderedBuildErrors
        errorCodes = New-OrderedObject $orderedErrorCodes
        buildErrorRecordCount = [int]$buildErrorState.recordCount
        errorCodeRecordCount = [int]$errorCodeState.recordCount
        failureRecordCount = [int]($buildErrorState.recordCount + $errorCodeState.recordCount)
        buildErrorsTruncated = [bool]($buildErrorState.recordsTruncated -or $buildErrorState.codesTruncated)
        errorCodesTruncated = [bool]($errorCodeState.recordsTruncated -or $errorCodeState.codesTruncated)
        failureRecordsTruncated = [bool]$overallState.truncated
        parserFailed = [bool]$parserFailed
        versionMatched = [bool]$versionMatched
        versionProofAvailable = [bool](-not [string]::IsNullOrWhiteSpace($ExpectedVersionTag) -and
            $versionMatched -and -not $parserFailed -and -not $overallState.truncated)
    }))
}

function Add-ProcessOutputMetadata {
    param(
        [Parameter(Mandatory)][object] $Result,
        [Parameter(Mandatory)][string] $StdOutPath,
        [Parameter(Mandatory)][string] $StdErrPath,
        [AllowNull()][string] $ExpectedVersionTag
    )
    try {
        $metadata = Get-ProcessOutputMetadata -StdOutPath $StdOutPath -StdErrPath $StdErrPath -ExpectedVersionTag $ExpectedVersionTag
    } catch {
        $metadata = New-ProcessOutputMetadataFailure
    }
    $Result | Add-Member -NotePropertyName outputMetadata -NotePropertyValue $metadata -Force
    $Result | Add-Member -NotePropertyName outputMetadataParseFailed -NotePropertyValue ([bool]$metadata.parserFailed) -Force
    $Result | Add-Member -NotePropertyName outputMetadataTruncated -NotePropertyValue ([bool]$metadata.failureRecordsTruncated) -Force
    return $Result
}

function Get-ArtifactSnapshot {
    param(
        [Parameter(Mandatory)][string] $Workspace,
        [Parameter(Mandatory)][hashtable[]] $Artifacts
    )
    $result = [System.Collections.Generic.List[object]]::new()
    foreach ($artifact in $Artifacts) {
        $relative = [string]$artifact.relativePath
        $path = [IO.Path]::GetFullPath((Join-Path $Workspace ($relative.Replace('/', '\'))))
        if (-not (Test-PathBelow -Path $path -Root $Workspace)) {
            throw "artifact path escaped workspace: $relative"
        }
        $exists = [IO.File]::Exists($path)
        if ($exists) {
            $item = Get-Item -LiteralPath $path -Force
            if ($item -isnot [IO.FileInfo]) {
                throw "artifact is not a regular file: $relative"
            }
            [void]$result.Add((New-OrderedObject ([ordered]@{
                label = [string]$artifact.label
                relativePath = $relative.Replace('\', '/')
                exists = $true
                sizeBytes = [UInt64]$item.Length
                sha256 = Get-Sha256 $path
                lastWriteTimeUtc = $item.LastWriteTimeUtc.ToString('o', [Globalization.CultureInfo]::InvariantCulture)
            })))
        } else {
            [void]$result.Add((New-OrderedObject ([ordered]@{
                label = [string]$artifact.label
                relativePath = $relative.Replace('\', '/')
                exists = $false
                sizeBytes = $null
                sha256 = $null
                lastWriteTimeUtc = $null
            })))
        }
    }
    return $result.ToArray()
}

function Compare-ArtifactSnapshots {
    param(
        [Parameter(Mandatory)][AllowEmptyCollection()][object[]] $Before,
        [Parameter(Mandatory)][object[]] $After
    )
    $changes = [System.Collections.Generic.List[object]]::new()
    foreach ($beforeArtifact in $Before) {
        $afterArtifact = @($After | Where-Object { $_.label -eq $beforeArtifact.label })
        if ($afterArtifact.Count -ne 1) {
            [void]$changes.Add((New-OrderedObject ([ordered]@{
                label = [string]$beforeArtifact.label
                kind = 'missing_snapshot'
            })))
            continue
        }
        $afterArtifact = $afterArtifact[0]
        $fields = @('exists', 'sizeBytes', 'sha256', 'lastWriteTimeUtc')
        $changedFields = @($fields | Where-Object { [string]$beforeArtifact.$_ -ne [string]$afterArtifact.$_ })
        if ($changedFields.Count -gt 0) {
            [void]$changes.Add((New-OrderedObject ([ordered]@{
                label = [string]$beforeArtifact.label
                kind = 'changed'
                fields = $changedFields
            })))
        }
    }
    return $changes.ToArray()
}

function Get-ArtifactDefinitions {
    param(
        [Parameter(Mandatory)][string] $Platform,
        [Parameter(Mandatory)][string] $Configuration
    )
    $profile = if ($Configuration -eq 'Debug') { 'debug' } else { 'release' }
    $build = "build/$Platform/$Configuration"
    return @(
        @{ label = 'rust_archive'; relativePath = "$build/rust/native/x86_64-pc-windows-msvc/$profile/sakura_native_ffi.lib" }
        @{ label = 'rust_stamp'; relativePath = "$build/rust/native/x86_64-pc-windows-msvc/$profile/sakura_native_ffi.msbuild.stamp" }
        @{ label = 'provider_obj'; relativePath = "$build/sakura_core/OutputServiceRustProvider.obj" }
        @{ label = 'sakura_exe'; relativePath = "$Platform/$Configuration/sakura.exe" }
    )
}

function Get-ArtifactByLabel {
    param([Parameter(Mandatory)][object[]] $Snapshot, [Parameter(Mandatory)][string] $Label)
    $matches = @($Snapshot | Where-Object { $_.label -eq $Label })
    if ($matches.Count -ne 1) { throw "artifact snapshot is missing label: $Label" }
    return $matches[0]
}

function Assert-RequiredArtifacts {
    param([Parameter(Mandatory)][object[]] $Snapshot, [Parameter(Mandatory)][string] $Phase)
    $missing = @($Snapshot | Where-Object { -not $_.exists } | ForEach-Object { [string]$_.label })
    if ($missing.Count -gt 0) {
        return (New-OrderedObject ([ordered]@{
            type = 'missing_output'
            phase = $Phase
            missing = $missing
        }))
    }
    return (New-OrderedObject ([ordered]@{ type = 'ok'; phase = $Phase }))
}

function Get-SourcePathMention {
    param(
        [Parameter(Mandatory)][AllowEmptyString()][string] $Line,
        [Parameter(Mandatory)][string] $Workspace
    )
    $pattern = '(?i)(?:"(?<quoted>[^"\r\n]+\.(?:cpp|cxx|cc|c|rs))"|(?<bare>[^\s"\r\n]+\.(?:cpp|cxx|cc|c|rs)))(?![A-Za-z0-9_.-])'
    $mentions = [System.Collections.Generic.List[string]]::new()
    foreach ($match in [regex]::Matches($Line, $pattern)) {
        $value = if ($match.Groups['quoted'].Success) { $match.Groups['quoted'].Value } else { $match.Groups['bare'].Value }
        if ([string]::IsNullOrWhiteSpace($value)) { continue }
        $value = $value.Trim().Replace('/', '\')
        try {
            if ([IO.Path]::IsPathRooted($value)) {
                [void]$mentions.Add((Get-RelativePath -Path $value -Root $Workspace))
            } else {
                [void]$mentions.Add($value.TrimStart([char[]]'\').Replace('\', '/'))
            }
        } catch {
            [void]$mentions.Add($value.Replace('\', '/'))
        }
    }
    return @($mentions | Where-Object { $_ -and $_ -ne '<outside-workspace>' } | Select-Object -Unique)
}

function Get-ProjectFromLine {
    param(
        [Parameter(Mandatory)][AllowEmptyString()][string] $Line,
        [Parameter(Mandatory)][string] $Workspace
    )
    if ($Line -notmatch '(?i)"(?<project>[^"]+\.vcxproj)"') { return $null }
    $project = $Matches.project
    try {
        if (-not [IO.Path]::IsPathRooted($project)) {
            $project = Join-Path $Workspace $project
        }
        $relative = Get-RelativePath -Path $project -Root $Workspace
        if ($relative -eq '<outside-workspace>' -or [string]::IsNullOrWhiteSpace($relative)) { return $null }
        return $relative
    } catch {
        return $null
    }
}

function Add-BoundedUnexpectedToolName {
    param(
        [Parameter(Mandatory)][Collections.IDictionary] $Names,
        [Parameter(Mandatory)][string] $Name
    )
    if ($Name -notmatch '^[a-z0-9_.-]{1,64}\.exe$') { return $false }
    if ($Names.Contains($Name)) {
        $Names[$Name] = [int]$Names[$Name] + 1
        return $true
    }
    if ($Names.Count -ge $script:MaxUnexpectedToolNames) { return $false }
    $Names[$Name] = 1
    return $true
}

function Get-ActionClassifications {
    <# Classify direct tool invocations only; nested Tracker command echoes do
       not start with the executable and therefore cannot create false work. #>
    param(
        [Parameter(Mandatory)][string] $LogPath,
        [Parameter(Mandatory)][string] $Workspace,
        [Parameter(Mandatory)][string] $Phase
    )
    if (-not [IO.File]::Exists($LogPath)) {
        throw "diagnostic log is missing: $LogPath"
    }
    $item = Get-Item -LiteralPath $LogPath -Force
    if ($item -isnot [IO.FileInfo]) { throw "diagnostic log is not a regular file: $LogPath" }
    $actions = [System.Collections.Generic.List[object]]::new()
    $actionCounts = [ordered]@{}
    foreach ($knownKind in @(
        'cargo', 'cargo-preflight', 'rustc', 'cl', 'link', 'lib', 'rc', 'mt',
        'cmake', 'senp-tool', 'vcpkg-applocal', 'delete', 'unexpected_tool'
    )) {
        $actionCounts[$knownKind] = 0
    }
    $actionKindsSeen = @{}
    $unexpectedToolNames = [ordered]@{}
    $unexpectedToolNamesTruncated = $false
    $actionRecordCount = 0
    $retainedActionCount = 0
    $unretainedActionCount = 0
    $currentProject = $null
    # Accept bare tools plus quoted, drive-rooted, or UNC executable paths.
    # Compiler/linker tools require an executable suffix. Requiring the path
    # to begin the line prevents diagnostic prose such as `Using "CL" task
    # from assembly ...CppTasks.Common.dll` from being classified as work.
    $toolPattern = '(?i)^\s*(?:\d+>)?\s*(?:(?:"[^"\r\n]*[\\/])|(?:(?:[A-Za-z]:[\\/]|\\\\)[^"\r\n]*[\\/]))?"?(?<tool>cargo(?:\.exe)?|rustc\.exe|cl\.exe|link\.exe|lib\.exe|rc\.exe|mt\.exe|cmake\.exe|vcpkg\.exe|sakura-senp-tool\.exe|sakura-senp-host\.exe)"?\s+(?<arguments>.*)$'
    $directExecutablePattern = '(?i)^\s*(?:\d+>)?\s*"?(?<executable>(?:(?:[A-Za-z]:[\\/]|\\\\)[^"\r\n]*[\\/])?(?<tool>[A-Za-z0-9_.-]+\.exe))"?\s+(?<arguments>.*)$'
    # TrackedVCToolTask prints an absolute executable output item and can then
    # print a localized status sentence beginning with that same output path.
    # Remember only a bounded set of recent path+TaskId pairs so those status
    # lines are not mistaken for direct product invocations.
    $recentArtifactOutputs = @{}
    $recentArtifactOrder = [Collections.Generic.Queue[object]]::new()
    $maxRecentArtifactOutputs = 32
    $lineOrdinal = 0
    # A zero-byte diagnostic log is a valid parser input for self-tests and
    # must not turn 0..-1 into an invalid negative array index.
    $encoding = [Text.UTF8Encoding]::new($false, $false)
    $reader = New-Object IO.StreamReader($LogPath, $encoding, $true)
    try {
        while (($line = $reader.ReadLine()) -ne $null) {
            $lineOrdinal++
            $project = Get-ProjectFromLine -Line $line -Workspace $Workspace
            if ($null -ne $project) { $currentProject = $project }

            $action = $null
            if ($line -match $toolPattern) {
                $toolName = ([string]$Matches.tool).ToLowerInvariant()
                $tool = ([regex]::Replace($toolName, '(?i)\.exe$', '')).ToLowerInvariant()
                $arguments = [string]$Matches.arguments
                if ($arguments -match '^\(TaskId:\s*\d+\)\s*$') {
                    # An executable item printed by task-parameter diagnostics
                    # is not a child-process command line.
                    continue
                }
                $kind = $tool
                $operation = 'invoke'
                if ($tool -eq 'cargo' -and $arguments -match '(?i)(^|\s)--version(?:\s|$)') {
                    $kind = 'cargo-preflight'
                    $operation = 'version'
                } elseif ($tool -eq 'cargo' -and $arguments -match '(?i)(^|\s)build(?:\s|$)') {
                    $operation = 'build'
                } elseif ($tool -eq 'cmake') {
                    if ($arguments -match '(?i)(^|\s)--build(?:\s|$)') {
                        $operation = 'build'
                    } elseif ($arguments -match '(?i)(^|\s)-(?:A|B|S)(?:\s|$)') {
                        $operation = 'configure'
                    } else {
                        $kind = 'unexpected_tool'
                    }
                } elseif ($tool -eq 'vcpkg') {
                    if ($arguments -match '(?i)^\s*z-applocal(?:\s|$)') {
                        $kind = 'vcpkg-applocal'
                        $operation = 'copy-runtime-dependencies'
                    } else {
                        $kind = 'unexpected_tool'
                    }
                } elseif ($tool -eq 'sakura-senp-tool') {
                    if ($arguments -match '(?i)^\s*(?<verb>componentize|pack-builtin)(?:\s|$)') {
                        $kind = 'senp-tool'
                        $operation = $Matches.verb.ToLowerInvariant()
                    } elseif ($arguments -match '(?i)"(?:[A-Za-z]:[\\/]|\\\\)[^"\r\n]*\.exe".*\(TaskId:\s*\d+\)\s*$') {
                        # The Copy task prints a source executable followed by
                        # its destination executable. It is artifact metadata,
                        # not an invocation of the SENP command-line tool.
                        continue
                    } else {
                        $kind = 'unexpected_tool'
                    }
                } elseif ($tool -eq 'sakura-senp-host') {
                    if ($arguments -match '(?i)"(?:[A-Za-z]:[\\/]|\\\\)[^"\r\n]*\.exe".*\(TaskId:\s*\d+\)\s*$') {
                        continue
                    }
                    # The product build copies the host but never executes it.
                    # A future direct invocation is therefore unexpected until
                    # its build-time contract is made explicit.
                    $kind = 'unexpected_tool'
                }
                if ($kind -eq 'unexpected_tool' -and
                    -not (Add-BoundedUnexpectedToolName -Names $unexpectedToolNames -Name $toolName)) {
                    $unexpectedToolNamesTruncated = $true
                }
                $sourcePaths = Get-SourcePathMention -Line $line -Workspace $Workspace
                $action = New-OrderedObject ([ordered]@{
                    phase = $Phase
                    kind = $kind
                    operation = $operation
                    project = $currentProject
                    sourcePaths = $sourcePaths
                })
            } else {
                # Keep the evidence bounded to direct executable lines. Unknown
                # executable invocations are typed as unexpected_tool so
                # mutation phases cannot silently accept a new build tool.
                # Tracker/MSBuild plumbing is intentionally ignored; its command
                # echo is not a tool execution performed by this phase.
                if ($line -match $directExecutablePattern) {
                    $unknownExecutable = [string]$Matches.executable
                    $unknownTool = $Matches.tool.ToLowerInvariant()
                    $unknownArguments = [string]$Matches.arguments
                    # Diagnostic output can print an executable artifact path
                    # followed only by its task identifier. It is not a child
                    # process command line and therefore is not an action.
                    $taskOnlyMatch = [regex]::Match(
                        $unknownArguments,
                        '^\(TaskId:[ \t]*(?<id>\d+)\)[ \t]*$',
                        [Text.RegularExpressions.RegexOptions]::IgnoreCase
                    )
                    $isAbsoluteExecutable = $unknownExecutable -match '^(?:[A-Za-z]:[\\/]|\\\\)'
                    $normalizedExecutable = $unknownExecutable.Replace('/', '\').ToLowerInvariant()
                    if ($taskOnlyMatch.Success -and $isAbsoluteExecutable) {
                        $artifactKey = $taskOnlyMatch.Groups['id'].Value + '|' + $normalizedExecutable
                        $recentArtifactOutputs[$artifactKey] = $lineOrdinal
                        $recentArtifactOrder.Enqueue((New-OrderedObject ([ordered]@{
                            key = $artifactKey
                            line = $lineOrdinal
                        })))
                        while ($recentArtifactOrder.Count -gt $maxRecentArtifactOutputs) {
                            $expired = $recentArtifactOrder.Dequeue()
                            if ($recentArtifactOutputs.ContainsKey($expired.key) -and
                                [int]$recentArtifactOutputs[$expired.key] -eq [int]$expired.line) {
                                $recentArtifactOutputs.Remove($expired.key)
                            }
                        }
                    }
                    $taskSuffixMatch = [regex]::Match(
                        $unknownArguments,
                        '\(TaskId:[ \t]*(?<id>\d+)\)[ \t]*$',
                        [Text.RegularExpressions.RegexOptions]::IgnoreCase
                    )
                    $isTrackedArtifactStatus = $false
                    if (-not $taskOnlyMatch.Success -and $taskSuffixMatch.Success -and $isAbsoluteExecutable) {
                        $artifactKey = $taskSuffixMatch.Groups['id'].Value + '|' + $normalizedExecutable
                        if ($recentArtifactOutputs.ContainsKey($artifactKey)) {
                            $artifactLine = [int]$recentArtifactOutputs[$artifactKey]
                            $isTrackedArtifactStatus = $lineOrdinal -gt $artifactLine -and
                                ($lineOrdinal - $artifactLine) -le 4
                        }
                    }
                    $isArtifactMetadata = $taskOnlyMatch.Success -or $isTrackedArtifactStatus
                    if (-not $isArtifactMetadata -and
                        $unknownTool -notin @('tracker.exe', 'msbuild.exe', 'cmd.exe', 'conhost.exe', 'python.exe', 'py.exe', 'git.exe')) {
                        if (-not (Add-BoundedUnexpectedToolName -Names $unexpectedToolNames -Name $unknownTool)) {
                            $unexpectedToolNamesTruncated = $true
                        }
                        $action = New-OrderedObject ([ordered]@{
                            phase = $Phase
                            kind = 'unexpected_tool'
                            operation = 'invoke'
                            project = $currentProject
                            sourcePaths = @()
                        })
                    }
                }

                # MSBuild's Delete task prints this only when a file is actually
                # removed. A Task "Delete" header alone is not an observed mutation.
                if ($null -eq $action -and $line -match '(?i)\bDeleting\s+(?:file|files)\b') {
                    $action = New-OrderedObject ([ordered]@{
                        phase = $Phase
                        kind = 'delete'
                        operation = 'delete'
                        project = $currentProject
                        sourcePaths = @()
                    })
                }
            }
            if ($null -eq $action) { continue }
            $actionKind = [string]$action.kind
            $actionRecordCount++
            $actionCounts[$actionKind] = [int]$actionCounts[$actionKind] + 1
            $actionKindsSeen[$actionKind] = $true
            if ($actions.Count -lt $script:MaxRetainedActions) {
                [void]$actions.Add($action)
                $retainedActionCount++
            } else {
                $unretainedActionCount++
            }
        }
    } finally {
        $reader.Dispose()
    }
    $orderedKinds = @($actionKindsSeen.Keys | Sort-Object)
    $workActionCount = 0
    foreach ($workKind in $script:WorkActionKinds) { $workActionCount += [int]$actionCounts[$workKind] }
    return (New-OrderedObject ([ordered]@{
        actions = $actions.ToArray()
        actionCounts = (New-OrderedObject $actionCounts)
        actionKinds = $orderedKinds
        actionRecordCount = [int]$actionRecordCount
        retainedActionCount = [int]$retainedActionCount
        unretainedActionCount = [int]$unretainedActionCount
        actionsTruncated = [bool]($unretainedActionCount -gt 0)
        closureProofAvailable = [bool]($unretainedActionCount -eq 0)
        workActionCount = [int]$workActionCount
        unexpectedToolNames = (New-OrderedObject $unexpectedToolNames)
        unexpectedToolNamesTruncated = [bool]$unexpectedToolNamesTruncated
    }))
}

function Get-WorkActions {
    param([AllowNull()][AllowEmptyCollection()][object[]] $Actions = @())
    return @($Actions | Where-Object { $_.kind -in $script:WorkActionKinds })
}

function Get-ActionCounts {
    param([AllowNull()][AllowEmptyCollection()][object[]] $Actions = @())
    $counts = [ordered]@{}
    foreach ($kind in @(
        'cargo', 'cargo-preflight', 'rustc', 'cl', 'link', 'lib', 'rc', 'mt',
        'cmake', 'senp-tool', 'vcpkg-applocal', 'delete', 'unexpected_tool'
    )) {
        $counts[$kind] = [int]@($Actions | Where-Object { $_.kind -eq $kind }).Count
    }
    return (New-OrderedObject $counts)
}

function Get-ExplicitConsumerClosure {
    param(
        [Parameter(Mandatory)][AllowEmptyCollection()][object[]] $Actions,
        [Parameter(Mandatory)][AllowEmptyCollection()][string[]] $ExpectedConsumers,
        [Parameter(Mandatory)][string] $Phase,
        [switch] $ActionsTruncated
    )
    $expected = @($ExpectedConsumers | Sort-Object -Unique)
    if ($ActionsTruncated) {
        # A capped action list cannot prove that every link was attributed to
        # the explicit contract.  Fail closed instead of inferring consumers
        # from the retained prefix.
        return (New-OrderedObject ([ordered]@{
            type = 'unexpected_consumer'
            phase = $Phase
            expected = $expected
            actual = @()
            missing = @('<action-records-truncated>')
            unexpected = @('<action-records-truncated>')
        }))
    }
    $actual = @($Actions | Where-Object { $_.kind -eq 'link' } |
        ForEach-Object { [string]$_.project } |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
        Sort-Object -Unique)
    $unattributed = @($Actions | Where-Object { $_.kind -eq 'link' -and [string]::IsNullOrWhiteSpace([string]$_.project) }).Count
    $unexpected = @($actual | Where-Object { $_ -notin $expected })
    $missing = @($expected | Where-Object { $_ -notin $actual })
    if ($unattributed -gt 0) {
        $unexpected += '<unattributed-link>'
    }
    $type = if ($unexpected.Count -eq 0 -and $missing.Count -eq 0) { 'ok' } else { 'unexpected_consumer' }
    return (New-OrderedObject ([ordered]@{
        type = $type
        phase = $Phase
        expected = $expected
        actual = $actual
        missing = @($missing)
        unexpected = @($unexpected | Sort-Object -Unique)
    }))
}













function New-ObservedSurvivorResult {
    param(
        [Parameter(Mandatory)][string] $Phase,
        [AllowNull()][int] $ExitCode,
        [Parameter(Mandatory)][double] $DurationSeconds,
        [Parameter(Mandatory)][object[]] $Observed,
        [Parameter(Mandatory)][AllowEmptyCollection()][object[]] $Remaining
    )
    # Once a descendant was observed after the root exited, the invocation is
    # a survivor failure even when the exact cleanup pass removes it.  Keeping
    # that rule in one constructor prevents a successful cleanup from being
    # mistaken for a clean process lifetime.
    return (New-OrderedObject ([ordered]@{
        type = 'survivor'
        phase = $Phase
        exitCode = $ExitCode
        durationSeconds = [Math]::Round($DurationSeconds, 3)
        observedSurvivorCount = [int]$Observed.Count
        observedSurvivorNames = Get-ProcessNameCounts $Observed
        postCleanupSurvivorCount = [int]$Remaining.Count
        postCleanupSurvivorNames = Get-ProcessNameCounts $Remaining
    }))
}

function Complete-OwnedProcessResult {
    param(
        [Parameter(Mandatory)][object] $OwnedProcess,
        [Parameter(Mandatory)][object] $Result,
        [Parameter(Mandatory)][string] $Phase,
        [AllowNull()][object] $StdOutCopyTask,
        [AllowNull()][object] $StdErrCopyTask,
        [AllowNull()][object] $StdOutStream,
        [AllowNull()][object] $StdErrStream
    )
    $completionVerified = $true
    foreach ($copyTask in @($StdOutCopyTask, $StdErrCopyTask)) {
        if ($null -eq $copyTask) { continue }
        try {
            if (-not $copyTask.Wait(5000) -or $copyTask.IsFaulted -or $copyTask.IsCanceled) {
                $completionVerified = $false
            }
        } catch {
            $completionVerified = $false
        }
    }
    foreach ($stream in @($StdOutStream, $StdErrStream)) {
        if ($null -eq $stream) { continue }
        try {
            $stream.Flush()
            $stream.Dispose()
        } catch {
            $completionVerified = $false
        }
    }
    try {
        $OwnedProcess.DisposeVerified()
    } catch {
        $completionVerified = $false
    }
    if ($completionVerified) { return $Result }
    # Preserve every already-sanitized observation. Cleanup failure changes the
    # public result type, but must not erase an exit code, helper observation,
    # or the last known post-cleanup census from the underlying result.
    $copy = $Result.PSObject.Copy()
    $underlyingResultType = [string]$Result.type
    $copy | Add-Member -NotePropertyName type -NotePropertyValue 'process_error' -Force
    $copy | Add-Member -NotePropertyName phase -NotePropertyValue $Phase -Force
    $copy | Add-Member -NotePropertyName underlyingResultType -NotePropertyValue $underlyingResultType -Force
    $copy | Add-Member -NotePropertyName cleanup -NotePropertyValue 'unavailable' -Force
    $copy | Add-Member -NotePropertyName cleanupVerified -NotePropertyValue $false -Force
    $copy | Add-Member -NotePropertyName postCleanupKnown -NotePropertyValue (
        [bool]($Result.PSObject.Properties['postCleanupSurvivorCount'])
    ) -Force
    return $copy
}

function Resolve-ApplicationPath {
    param([Parameter(Mandatory)][string] $FilePath)
    if ($FilePath.IndexOf("`r", [StringComparison]::Ordinal) -ge 0 -or
        $FilePath.IndexOf("`n", [StringComparison]::Ordinal) -ge 0) {
        throw 'application path contains a line break'
    }
    if ([IO.Path]::IsPathRooted($FilePath)) {
        $resolved = Get-FullPath $FilePath
    } else {
        $commands = @(Get-Command -Name $FilePath -CommandType Application -ErrorAction Stop)
        if ($commands.Count -eq 0) { throw 'application was not found' }
        # Get-Command can return the same executable name from more than one
        # PATH entry. Match ordinary command resolution by selecting the first
        # application instead of stringifying and concatenating the array.
        $resolved = Get-FullPath ([string]$commands[0].Source)
    }
    if (-not [IO.File]::Exists($resolved)) { throw 'application path does not exist' }
    return $resolved
}



function Invoke-BoundedProcess {
    param(
        [Parameter(Mandatory)][string] $FilePath,
        [Parameter(Mandatory)][string[]] $ArgumentList,
        [Parameter(Mandatory)][string] $WorkingDirectory,
        [Parameter(Mandatory)][string] $StdOutPath,
        [Parameter(Mandatory)][string] $StdErrPath,
        [Parameter(Mandatory)][int] $TimeoutSeconds,
        [Parameter(Mandatory)][string] $Phase,
        [AllowNull()][AllowEmptyCollection()][string[]] $AllowedPostExitHelperNames = @()
    )
    $start = [Diagnostics.Stopwatch]::StartNew()
    $process = $null
    $stdoutStream = $null
    $stderrStream = $null
    $stdoutCopyTask = $null
    $stderrCopyTask = $null
    try {
        Initialize-OwnedProcessInterop
        $applicationPath = Resolve-ApplicationPath $FilePath
        $commandLine = ConvertTo-WindowsCommandLine -Arguments (@($applicationPath) + @($ArgumentList))
        $stdoutStream = [IO.File]::Open($StdOutPath, [IO.FileMode]::Create, [IO.FileAccess]::Write, [IO.FileShare]::Read)
        $stderrStream = [IO.File]::Open($StdErrPath, [IO.FileMode]::Create, [IO.FileAccess]::Write, [IO.FileShare]::Read)
        # CreateProcessW applies the private kill-on-close Job Object and the
        # exact three inherited stdio handles atomically through STARTUPINFOEX.
        # The target is also suspended until parent-side setup is complete, so
        # no target instruction or descendant can run outside the owned job.
        $process = [Sakura.NativeRustVerifier.OwnedProcess]::Start(
            $applicationPath, $commandLine, $WorkingDirectory)
        $stdoutCopyTask = $process.StandardOutputStream.CopyToAsync($stdoutStream)
        $stderrCopyTask = $process.StandardErrorStream.CopyToAsync($stderrStream)
        $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
        $timedOut = $false
        while (-not $process.HasExited) {
            if ([DateTime]::UtcNow -ge $deadline) {
                $timedOut = $true
                break
            }
            [void]$process.WaitForExit(250)
        }
        if ($timedOut) {
            $observedBeforeStop = @(Get-OwnedJobMemberRecords -OwnedProcess $process)
            $process.Kill()
            $jobEmpty = $process.WaitForJobEmpty(5000)
            $remaining = @(Get-OwnedJobMemberRecords -OwnedProcess $process)
            $start.Stop()
            if (-not $jobEmpty -or $remaining.Count -gt 0) {
                $result = New-OrderedObject ([ordered]@{
                    type = 'survivor'
                    phase = $Phase
                    exitCode = $null
                    durationSeconds = [Math]::Round($start.Elapsed.TotalSeconds, 3)
                    timedOut = $true
                    observedSurvivorCount = [int]$observedBeforeStop.Count
                    observedSurvivorNames = Get-ProcessNameCounts $observedBeforeStop
                    postCleanupSurvivorCount = [int]$remaining.Count
                    postCleanupSurvivorNames = Get-ProcessNameCounts $remaining
                })
                return (Complete-OwnedProcessResult -OwnedProcess $process -Result $result -Phase $Phase `
                    -StdOutCopyTask $stdoutCopyTask -StdErrCopyTask $stderrCopyTask `
                    -StdOutStream $stdoutStream -StdErrStream $stderrStream)
            }
            $result = New-OrderedObject ([ordered]@{
                type = 'timeout'
                phase = $Phase
                exitCode = $null
                durationSeconds = [Math]::Round($start.Elapsed.TotalSeconds, 3)
                observedProcessCount = [int]$observedBeforeStop.Count
                observedProcessNames = Get-ProcessNameCounts $observedBeforeStop
                postCleanupSurvivorCount = 0
                postCleanupSurvivorNames = Get-ProcessNameCounts @()
            })
            return (Complete-OwnedProcessResult -OwnedProcess $process -Result $result -Phase $Phase `
                -StdOutCopyTask $stdoutCopyTask -StdErrCopyTask $stderrCopyTask `
                -StdOutStream $stdoutStream -StdErrStream $stderrStream)
        }

        $process.WaitForExit()
        $exitCode = [int]$process.ExitCode
        if (-not $process.WaitForProcessRemoval($process.Id, 1000)) {
            throw 'exited root remained in the Job Object process list'
        }
        # A child can be signaled but remain in the kernel job list briefly
        # while its final handles unwind. Give ordinary descendants a bounded
        # grace period; persistent compiler helpers remain visible afterwards.
        if ($process.WaitForJobEmpty(1000)) {
            $activeMembers = @()
        } else {
            $activeMembers = @(Get-OwnedJobMemberRecords -OwnedProcess $process)
        }
        $expectedHelpers = @()
        $unexpectedHelpers = @()
        $remaining = @()
        if ($activeMembers.Count -gt 0) {
            $disposition = Get-OwnedJobMemberDisposition -Records $activeMembers `
                -AllowedHelperNames $AllowedPostExitHelperNames
            $expectedHelpers = @($disposition.expected)
            $unexpectedHelpers = @($disposition.unexpected)
            # TerminateJobObject acts on the kernel-owned process set, not on a
            # PID looked up in a racy user-mode census.
            $process.Kill()
            $jobEmpty = $process.WaitForJobEmpty(5000)
            $remaining = @(Get-OwnedJobMemberRecords -OwnedProcess $process)
            if (-not $jobEmpty -and $remaining.Count -eq 0) {
                throw 'job cleanup did not reach a stable empty state'
            }
        }

        # Descendants can inherit redirected handles. Drain only after the job
        # is empty so an accepted compiler helper cannot pin either log stream.
        foreach ($copyTask in @($stdoutCopyTask, $stderrCopyTask)) {
            if ($null -eq $copyTask) { continue }
            if (-not $copyTask.Wait(5000)) { throw 'redirected process output did not drain' }
            if ($copyTask.IsFaulted -or $copyTask.IsCanceled) { throw 'redirected process output failed' }
        }
        if ($null -ne $stdoutStream) { $stdoutStream.Flush(); $stdoutStream.Dispose(); $stdoutStream = $null }
        if ($null -ne $stderrStream) { $stderrStream.Flush(); $stderrStream.Dispose(); $stderrStream = $null }
        $start.Stop()

        if ($unexpectedHelpers.Count -gt 0 -or $remaining.Count -gt 0) {
            $observed = if ($unexpectedHelpers.Count -gt 0) { $unexpectedHelpers } else { $remaining }
            $result = New-ObservedSurvivorResult -Phase $Phase -ExitCode $exitCode `
                -DurationSeconds $start.Elapsed.TotalSeconds -Observed $observed -Remaining $remaining
            $result | Add-Member -NotePropertyName observedExpectedHelperCount -NotePropertyValue ([int]$expectedHelpers.Count)
            $result | Add-Member -NotePropertyName observedExpectedHelperNames -NotePropertyValue (Get-ProcessNameCounts $expectedHelpers)
            return (Complete-OwnedProcessResult -OwnedProcess $process -Result $result -Phase $Phase `
                -StdOutCopyTask $stdoutCopyTask -StdErrCopyTask $stderrCopyTask `
                -StdOutStream $stdoutStream -StdErrStream $stderrStream)
        }
        if ($exitCode -ne 0) {
            $result = New-OrderedObject ([ordered]@{
                type = 'build_failed'
                phase = $Phase
                exitCode = $exitCode
                durationSeconds = [Math]::Round($start.Elapsed.TotalSeconds, 3)
                outputAvailable = [IO.File]::Exists($StdOutPath) -or [IO.File]::Exists($StdErrPath)
                observedExpectedHelperCount = [int]$expectedHelpers.Count
                observedExpectedHelperNames = Get-ProcessNameCounts $expectedHelpers
                postCleanupSurvivorCount = 0
                postCleanupSurvivorNames = Get-ProcessNameCounts @()
            })
            return (Complete-OwnedProcessResult -OwnedProcess $process -Result $result -Phase $Phase `
                -StdOutCopyTask $stdoutCopyTask -StdErrCopyTask $stderrCopyTask `
                -StdOutStream $stdoutStream -StdErrStream $stderrStream)
        }
        if (-not ([IO.File]::Exists($StdOutPath) -or [IO.File]::Exists($StdErrPath))) {
            $result = New-OrderedObject ([ordered]@{
                type = 'missing_output'
                phase = $Phase
                exitCode = $exitCode
                durationSeconds = [Math]::Round($start.Elapsed.TotalSeconds, 3)
                observedExpectedHelperCount = [int]$expectedHelpers.Count
                observedExpectedHelperNames = Get-ProcessNameCounts $expectedHelpers
                postCleanupSurvivorCount = 0
                postCleanupSurvivorNames = Get-ProcessNameCounts @()
            })
            return (Complete-OwnedProcessResult -OwnedProcess $process -Result $result -Phase $Phase `
                -StdOutCopyTask $stdoutCopyTask -StdErrCopyTask $stderrCopyTask `
                -StdOutStream $stdoutStream -StdErrStream $stderrStream)
        }
        $result = New-OrderedObject ([ordered]@{
            type = 'ok'
            phase = $Phase
            exitCode = $exitCode
            durationSeconds = [Math]::Round($start.Elapsed.TotalSeconds, 3)
            observedExpectedHelperCount = [int]$expectedHelpers.Count
            observedExpectedHelperNames = Get-ProcessNameCounts $expectedHelpers
            postCleanupSurvivorCount = 0
            postCleanupSurvivorNames = Get-ProcessNameCounts @()
        })
        return (Complete-OwnedProcessResult -OwnedProcess $process -Result $result -Phase $Phase `
            -StdOutCopyTask $stdoutCopyTask -StdErrCopyTask $stderrCopyTask `
            -StdOutStream $stdoutStream -StdErrStream $stderrStream)
    } catch {
        $start.Stop()
        $cleanupType = if ($null -eq $process) { 'not_started' } else { 'unavailable' }
        $observed = @()
        $remaining = @()
        if ($null -ne $process) {
            try {
                $observed = @(Get-OwnedJobMemberRecords -OwnedProcess $process)
                $process.Kill()
                $jobEmpty = $process.WaitForJobEmpty(5000)
                $remaining = @(Get-OwnedJobMemberRecords -OwnedProcess $process)
                $cleanupType = if ($jobEmpty -and $remaining.Count -eq 0) { 'verified' } else { 'survivor' }
            } catch {
                $cleanupType = 'unavailable'
            }
        }
        $result = New-OrderedObject ([ordered]@{
            type = 'process_error'
            phase = $Phase
            exitCode = $null
            durationSeconds = [Math]::Round($start.Elapsed.TotalSeconds, 3)
            cleanup = $cleanupType
            observedProcessCount = [int]$observed.Count
            observedProcessNames = Get-ProcessNameCounts $observed
            postCleanupSurvivorCount = [int]$remaining.Count
            postCleanupSurvivorNames = Get-ProcessNameCounts $remaining
        })
        if ($null -eq $process) { return $result }
        return (Complete-OwnedProcessResult -OwnedProcess $process -Result $result -Phase $Phase `
            -StdOutCopyTask $stdoutCopyTask -StdErrCopyTask $stderrCopyTask `
            -StdOutStream $stdoutStream -StdErrStream $stderrStream)
    } finally {
        $copyWaitMilliseconds = 5000
        foreach ($copyTask in @($stdoutCopyTask, $stderrCopyTask)) {
            if ($null -eq $copyTask) { continue }
            try { [void]$copyTask.Wait($copyWaitMilliseconds) } catch { }
        }
        foreach ($stream in @($stdoutStream, $stderrStream)) {
            if ($null -ne $stream) { try { $stream.Dispose() } catch { } }
        }
        if ($null -ne $process) { try { $process.Dispose() } catch { } }
    }
}

function Join-ProcessOutput {
    param(
        [Parameter(Mandatory)][string] $StdOutPath,
        [Parameter(Mandatory)][string] $StdErrPath,
        [Parameter(Mandatory)][string] $CombinedPath
    )
    $parts = [System.Collections.Generic.List[string]]::new()
    foreach ($path in @($StdOutPath, $StdErrPath)) {
        if ([IO.File]::Exists($path)) {
            [void]$parts.Add([IO.File]::ReadAllText($path))
        }
    }
    [IO.File]::WriteAllText($CombinedPath, ($parts -join "`r`n"), (New-Object Text.UTF8Encoding($false)))
}

function Get-MSBuildPath {
    $command = Get-Command msbuild.exe -ErrorAction SilentlyContinue
    if ($command) { return (Get-FullPath $command.Source) }
    $candidates = [System.Collections.Generic.List[string]]::new()
    $vswhere = Get-Command vswhere.exe -ErrorAction SilentlyContinue
    if ($vswhere) {
        $installations = & $vswhere.Source -latest -products * -requires Microsoft.Component.MSBuild -property installationPath 2>$null
        foreach ($installation in @($installations)) {
            if ([string]::IsNullOrWhiteSpace([string]$installation)) { continue }
            [void]$candidates.Add((Join-Path ([string]$installation).Trim() 'MSBuild\Current\Bin\MSBuild.exe'))
            [void]$candidates.Add((Join-Path ([string]$installation).Trim() 'MSBuild\15.0\Bin\MSBuild.exe'))
        }
    }
    foreach ($programFiles in @($env:ProgramFiles, ${env:ProgramFiles(x86)}) | Where-Object { $_ }) {
        foreach ($version in @('2022', '2019', '2017')) {
            foreach ($edition in @('Enterprise', 'Professional', 'Community', 'BuildTools')) {
                [void]$candidates.Add((Join-Path $programFiles "Microsoft Visual Studio\$version\$edition\MSBuild\Current\Bin\MSBuild.exe"))
            }
        }
        [void]$candidates.Add((Join-Path $programFiles 'MSBuild\Current\Bin\MSBuild.exe'))
    }
    foreach ($candidate in @($candidates.ToArray() | Where-Object { $_ } | Select-Object -Unique)) {
        if ([IO.File]::Exists($candidate)) { return (Get-FullPath $candidate) }
    }
    throw 'MSBuild.exe was not found on PATH, through vswhere, or in known Visual Studio locations.'
}

function Get-PayloadFreeFileMetadata {
    param([Parameter(Mandatory)][string] $Path)
    if (-not [IO.File]::Exists($Path)) {
        return (New-OrderedObject ([ordered]@{
            exists = $false
            sizeBytes = $null
            sha256 = $null
        }))
    }
    $item = Get-Item -LiteralPath $Path -Force
    if ($item -isnot [IO.FileInfo]) { throw 'expected a regular file' }
    return (New-OrderedObject ([ordered]@{
        exists = $true
        sizeBytes = [UInt64]$item.Length
        sha256 = Get-Sha256 $Path
    }))
}

function Get-VcpkgPinnedReleaseTag {
    param([Parameter(Mandatory)][string] $Workspace)
    $metadata = Join-Path $Workspace 'tools/vcpkg/scripts/vcpkg-tool-metadata.txt'
    Assert-PathBelow -Path $metadata -Root $Workspace -Context 'isolated vcpkg metadata' | Out-Null
    Assert-NoReparsePoint -Path $metadata -Context 'isolated vcpkg metadata'
    if (-not [IO.File]::Exists($metadata)) { throw 'isolated vcpkg metadata is missing' }
    $text = [IO.File]::ReadAllText($metadata, [Text.Encoding]::ASCII)
    $match = [regex]::Match($text, '(?m)^\s*VCPKG_TOOL_RELEASE_TAG\s*=\s*(?<tag>[A-Za-z0-9][A-Za-z0-9._-]{0,63})\s*$')
    if (-not $match.Success) { throw 'isolated vcpkg release tag is missing or invalid' }
    return $match.Groups['tag'].Value
}

function Get-IsolatedVcpkgRoot {
    param([Parameter(Mandatory)][string] $Workspace)
    Assert-NoReparsePoint -Path $Workspace -Context 'isolated vcpkg workspace'
    $root = Join-Path $Workspace 'tools/vcpkg'
    Assert-PathBelow -Path $root -Root $Workspace -Context 'isolated vcpkg root' | Out-Null
    return (Get-FullPath $root)
}

function New-VcpkgToolFailureResult {
    param(
        [Parameter(Mandatory)][string] $ReasonCode,
        [AllowNull()][string] $ExpectedReleaseTag,
        [AllowNull()][object] $Source,
        [AllowNull()][object] $Isolated,
        [AllowNull()][object] $VersionValidation,
        [AllowNull()][object] $VersionProbe,
        [AllowNull()][object] $DisableMetricsMarker
    )
    return (New-OrderedObject ([ordered]@{
        type = 'build_failed'
        phase = 'package_tool'
        exitCode = 3
        validation = 'failed'
        reasonCode = $ReasonCode
        expectedReleaseTag = $ExpectedReleaseTag
        source = $Source
        isolated = $Isolated
        versionValidation = $VersionValidation
        versionProbe = $VersionProbe
        disableMetricsMarker = $DisableMetricsMarker
    }))
}

function Prepare-IsolatedVcpkgTool {
    param(
        [Parameter(Mandatory)][string] $RepositoryRoot,
        [Parameter(Mandatory)][string] $Workspace,
        [Parameter(Mandatory)][int] $TimeoutSeconds
    )
    $tag = $null
    $source = New-OrderedObject ([ordered]@{ exists = $false; sizeBytes = $null; sha256 = $null })
    $isolated = New-OrderedObject ([ordered]@{ exists = $false; sizeBytes = $null; sha256 = $null })
    $disableMetricsMarker = New-OrderedObject ([ordered]@{ exists = $false; sizeBytes = $null; sha256 = $null })
    $versionValidation = $null
    $versionProbe = $null
    $stage = 'metadata'
    try {
        Assert-NoReparsePoint -Path $Workspace -Context 'isolated vcpkg workspace'
        $tag = Get-VcpkgPinnedReleaseTag -Workspace $Workspace

        $stage = 'source'
        $sourcePath = Join-Path $RepositoryRoot 'tools/vcpkg/vcpkg.exe'
        Assert-PathBelow -Path $sourcePath -Root $RepositoryRoot -Context 'shared vcpkg tool' | Out-Null
        Assert-NoReparsePoint -Path $sourcePath -Context 'shared vcpkg tool'
        $source = Get-PayloadFreeFileMetadata -Path $sourcePath
        if (-not $source.exists) { throw 'shared vcpkg tool is missing' }

        $isolatedVcpkgRoot = Get-IsolatedVcpkgRoot -Workspace $Workspace
        if (-not [IO.Directory]::Exists($isolatedVcpkgRoot)) {
            New-Item -ItemType Directory -Path $isolatedVcpkgRoot -Force | Out-Null
        }
        Assert-NoReparsePoint -Path $isolatedVcpkgRoot -Context 'isolated vcpkg root'
        $isolatedToolPath = Join-Path $isolatedVcpkgRoot 'vcpkg.exe'
        Assert-PathBelow -Path $isolatedToolPath -Root $Workspace -Context 'isolated vcpkg tool' | Out-Null
        if ([IO.File]::Exists($isolatedToolPath)) { Assert-NoReparsePoint -Path $isolatedToolPath -Context 'isolated vcpkg tool' }

        $logDirectory = Join-Path $Workspace 'build/logs/native-rust-incremental'
        Assert-PathBelow -Path $logDirectory -Root $Workspace -Context 'vcpkg tool log directory' | Out-Null
        New-Item -ItemType Directory -Path $logDirectory -Force | Out-Null
        Assert-NoReparsePoint -Path $logDirectory -Context 'vcpkg tool log directory'
        $probeStdOut = Join-Path $logDirectory 'vcpkg-tool-version.stdout.log'
        $probeStdErr = Join-Path $logDirectory 'vcpkg-tool-version.stderr.log'
        $versionProbe = Invoke-BoundedProcess -FilePath $sourcePath `
            -ArgumentList @('version', '--disable-metrics') `
            -WorkingDirectory (Split-Path -Parent $sourcePath) -StdOutPath $probeStdOut -StdErrPath $probeStdErr `
            -TimeoutSeconds $TimeoutSeconds -Phase 'vcpkg_tool_version'
        $versionProbe = Add-ProcessOutputMetadata -Result $versionProbe -StdOutPath $probeStdOut -StdErrPath $probeStdErr -ExpectedVersionTag $tag
        $versionValidation = New-OrderedObject ([ordered]@{
            expectedReleaseTag = $tag
            reportedVersionMatched = [bool]$versionProbe.outputMetadata.versionMatched
            proofAvailable = [bool]$versionProbe.outputMetadata.versionProofAvailable
            parserFailed = [bool]$versionProbe.outputMetadata.parserFailed
            recordsTruncated = [bool]$versionProbe.outputMetadata.failureRecordsTruncated
            probeType = [string]$versionProbe.type
            probeExitCode = $versionProbe.exitCode
        })
        if ($versionProbe.type -ne 'ok') { $stage = 'probe'; throw 'vcpkg version probe failed' }
        if (-not $versionValidation.proofAvailable) { $stage = 'validate'; throw 'vcpkg version did not prove the pinned release' }

        $stage = 'source'
        $sourceAfterProbe = Get-PayloadFreeFileMetadata -Path $sourcePath
        if (-not $sourceAfterProbe.exists -or $sourceAfterProbe.sizeBytes -ne $source.sizeBytes -or
            $sourceAfterProbe.sha256 -ne $source.sha256) {
            throw 'shared vcpkg tool changed during validation'
        }
        $source = $sourceAfterProbe

        $stage = 'copy'
        # This is deliberately a byte-for-byte copy of the already validated
        # shared tool.  No bootstrap script or network download is permitted.
        [IO.File]::Copy($sourcePath, $isolatedToolPath, $true)
        Assert-NoReparsePoint -Path $isolatedToolPath -Context 'isolated vcpkg tool'
        $isolated = Get-PayloadFreeFileMetadata -Path $isolatedToolPath
        if (-not $isolated.exists -or $isolated.sizeBytes -ne $source.sizeBytes -or $isolated.sha256 -ne $source.sha256) {
            throw 'isolated vcpkg tool copy did not match the validated source'
        }

        $stage = 'marker'
        $markerPath = Join-Path $isolatedVcpkgRoot 'vcpkg.disable-metrics'
        Assert-PathBelow -Path $markerPath -Root $Workspace -Context 'isolated vcpkg metrics marker' | Out-Null
        [IO.File]::WriteAllText($markerPath, '', (New-Object Text.UTF8Encoding($false)))
        Assert-NoReparsePoint -Path $markerPath -Context 'isolated vcpkg metrics marker'
        $disableMetricsMarker = Get-PayloadFreeFileMetadata -Path $markerPath
        if (-not $disableMetricsMarker.exists -or $disableMetricsMarker.sizeBytes -ne 0) {
            throw 'isolated vcpkg metrics marker was not created'
        }

        return (New-OrderedObject ([ordered]@{
            type = 'ok'
            phase = 'package_tool'
            exitCode = 0
            validation = 'matched'
            reasonCode = $null
            expectedReleaseTag = $tag
            source = $source
            isolated = $isolated
            versionValidation = $versionValidation
            versionProbe = $versionProbe
            disableMetricsMarker = $disableMetricsMarker
        }))
    } catch {
        $reasonCode = switch ($stage) {
            'metadata' { 'TOOL_VCPKG_METADATA_INVALID' }
            'source' { 'TOOL_VCPKG_SOURCE_INVALID' }
            'probe' { 'TOOL_VCPKG_VERSION_PROBE_FAILED' }
            'validate' { 'TOOL_VCPKG_VERSION_MISMATCH' }
            'copy' { 'TOOL_VCPKG_COPY_FAILED' }
            'marker' { 'TOOL_VCPKG_MARKER_FAILED' }
            default { 'TOOL_VCPKG_PREPARE_FAILED' }
        }
        return (New-VcpkgToolFailureResult -ReasonCode $reasonCode -ExpectedReleaseTag $tag `
            -Source $source -Isolated $isolated -VersionValidation $versionValidation -VersionProbe $versionProbe `
            -DisableMetricsMarker $disableMetricsMarker)
    }
}

function Invoke-PackageRestore {
    param(
        [Parameter(Mandatory)][string] $Workspace,
        [Parameter(Mandatory)][string] $Configuration,
        [Parameter(Mandatory)][int] $TimeoutSeconds
    )
    $logDirectory = Join-Path $Workspace 'build/logs/native-rust-incremental'
    New-Item -ItemType Directory -Path $logDirectory -Force | Out-Null
    $stdout = Join-Path $logDirectory 'package-restore.stdout.log'
    $stderr = Join-Path $logDirectory 'package-restore.stderr.log'
    $pythonLauncher = Get-Command py.exe -ErrorAction SilentlyContinue
    $pythonFile = if ($pythonLauncher) { $pythonLauncher.Source } else { 'py.exe' }
    $context = "msvc-x64-$($Configuration.ToLowerInvariant())"
    $result = Invoke-BoundedProcess -FilePath $pythonFile `
        -ArgumentList @('-3', 'tools/build/sakura_build.py', 'package', 'restore', 'sakura_app', '--context', $context, '--timeout-seconds', [string]$TimeoutSeconds) `
        -WorkingDirectory $Workspace -StdOutPath $stdout -StdErrPath $stderr `
        -TimeoutSeconds $TimeoutSeconds -Phase 'package_restore'
    $result = Add-ProcessOutputMetadata -Result $result -StdOutPath $stdout -StdErrPath $stderr
    $result | Add-Member -NotePropertyName outputAvailable -NotePropertyValue ([bool]$result.outputMetadata.outputAvailable) -Force
    return $result
}

function Invoke-MsbuildPhase {
    param(
        [Parameter(Mandatory)][string] $Workspace,
        [Parameter(Mandatory)][string] $Phase,
        [Parameter(Mandatory)][string] $Platform,
        [Parameter(Mandatory)][string] $Configuration,
        [Parameter(Mandatory)][int] $TimeoutSeconds
    )
    $logDirectory = Join-Path $Workspace 'build/logs/native-rust-incremental'
    New-Item -ItemType Directory -Path $logDirectory -Force | Out-Null
    $stdout = Join-Path $logDirectory "$Phase.stdout.log"
    $stderr = Join-Path $logDirectory "$Phase.stderr.log"
    $combined = Join-Path $logDirectory "$Phase.log"
    $project = Join-Path $Workspace 'sakura_core/sakura.vcxproj'
    if (-not [IO.File]::Exists($project)) { throw "MSBuild project is missing: $project" }
    Assert-NoReparsePoint -Path $project -Context 'MSBuild project'
    $msbuild = Get-MSBuildPath
    # Run the product project directly.  The canonical build-dev wrapper also
    # performs package restore, but it cannot carry a per-phase diagnostic file
    # logger.  Package restore is an explicit bounded predecessor instead.
    $arguments = @(
        $project,
        "/p:Platform=$Platform",
        "/p:Configuration=$Configuration",
        '/t:Build',
        '/nr:false',
        '/m:1',
        '/p:MultiProcessorCompilation=true',
        '/p:CL_MPCount=1',
        '/verbosity:diagnostic',
        '/nologo',
        ("/flp:logfile=$combined;verbosity=diagnostic;Encoding=UTF-8")
    )
    $processResult = Invoke-BoundedProcess -FilePath $msbuild `
        -ArgumentList $arguments `
        -WorkingDirectory $Workspace -StdOutPath $stdout -StdErrPath $stderr `
        -TimeoutSeconds $TimeoutSeconds -Phase $Phase `
        -AllowedPostExitHelperNames @('mspdbsrv.exe')
    # The file logger is the authoritative diagnostic stream.  Preserve it;
    # only synthesize a combined fallback when MSBuild failed before opening
    # the logger file.
    if (-not [IO.File]::Exists($combined)) {
        Join-ProcessOutput -StdOutPath $stdout -StdErrPath $stderr -CombinedPath $combined
    }
    $logAvailable = [IO.File]::Exists($combined)
    $diagnostics = $null
    $classification = $null
    $actions = @()
    $diagnosticParseFailed = $false
    if ($logAvailable) {
        try {
            $diagnostics = Get-DiagnosticLogSummary -LogPath $combined
        } catch {
            $diagnosticParseFailed = $true
        }
        try {
            $classification = Get-ActionClassifications -LogPath $combined -Workspace $Workspace -Phase $Phase
            $actions = @($classification.actions)
        } catch {
            # Keep the underlying process result and avoid leaking parser
            # exception text (which can contain paths or command payload).
            $diagnosticParseFailed = $true
        }
    }
    $actionCounts = if ($null -ne $classification) { $classification.actionCounts } else { Get-ActionCounts @() }
    $actionKinds = if ($null -ne $classification) { @($classification.actionKinds) } else { @() }
    $actionRecordCount = if ($null -ne $classification) { [int]$classification.actionRecordCount } else { 0 }
    $retainedActionCount = if ($null -ne $classification) { [int]$classification.retainedActionCount } else { 0 }
    $unretainedActionCount = if ($null -ne $classification) { [int]$classification.unretainedActionCount } else { 0 }
    $actionsTruncated = if ($null -ne $classification) { [bool]$classification.actionsTruncated } else { $false }
    $closureProofAvailable = if ($null -ne $classification) { [bool]$classification.closureProofAvailable } else { $false }
    $workActionCount = if ($null -ne $classification) { [int]$classification.workActionCount } else { 0 }
    $unexpectedToolNames = if ($null -ne $classification) { $classification.unexpectedToolNames } else { New-OrderedObject ([ordered]@{}) }
    $unexpectedToolNamesTruncated = if ($null -ne $classification) { [bool]$classification.unexpectedToolNamesTruncated } else { $false }
    if ($processResult.type -ne 'ok') {
        # A build failure and a process survivor are independent facts.  Keep
        # the bounded process result as the terminal type even when parsing
        # diagnostics also fails, so cleanup evidence cannot erase the build
        # cause.  Parser status is a separate payload-free boolean.
        return (New-OrderedObject ([ordered]@{
            name = $Phase
            result = $processResult
            diagnostics = $diagnostics
            diagnosticsParseFailed = [bool]$diagnosticParseFailed
            actionCounts = $actionCounts
            actionKinds = $actionKinds
            actions = $actions
            actionRecordCount = $actionRecordCount
            retainedActionCount = $retainedActionCount
            unretainedActionCount = $unretainedActionCount
            actionsTruncated = $actionsTruncated
            closureProofAvailable = $closureProofAvailable
            workActionCount = $workActionCount
            unexpectedToolNames = $unexpectedToolNames
            unexpectedToolNamesTruncated = $unexpectedToolNamesTruncated
            logAvailable = $logAvailable
        }))
    }
    if (-not $logAvailable) {
        return (New-OrderedObject ([ordered]@{
            name = $Phase
            result = (New-OrderedObject ([ordered]@{ type = 'missing_output'; phase = $Phase }))
            diagnostics = $null
            diagnosticsParseFailed = $false
            actionCounts = $actionCounts
            actionKinds = @()
            actions = @()
            actionRecordCount = $actionRecordCount
            retainedActionCount = $retainedActionCount
            unretainedActionCount = $unretainedActionCount
            actionsTruncated = $actionsTruncated
            closureProofAvailable = $closureProofAvailable
            workActionCount = 0
            unexpectedToolNames = $unexpectedToolNames
            unexpectedToolNamesTruncated = $unexpectedToolNamesTruncated
            logAvailable = $false
        }))
    }
    if ($diagnosticParseFailed) {
        return (New-OrderedObject ([ordered]@{
            name = $Phase
            result = (New-OrderedObject ([ordered]@{ type = 'process_error'; phase = $Phase }))
            diagnostics = $diagnostics
            diagnosticsParseFailed = $true
            actionCounts = $actionCounts
            actionKinds = @()
            actions = @()
            actionRecordCount = $actionRecordCount
            retainedActionCount = $retainedActionCount
            unretainedActionCount = $unretainedActionCount
            actionsTruncated = $actionsTruncated
            closureProofAvailable = $closureProofAvailable
            workActionCount = 0
            unexpectedToolNames = $unexpectedToolNames
            unexpectedToolNamesTruncated = $unexpectedToolNamesTruncated
            logAvailable = $true
        }))
    }
    return (New-OrderedObject ([ordered]@{
        name = $Phase
        result = $processResult
        diagnostics = $diagnostics
        diagnosticsParseFailed = $false
        actionCounts = $actionCounts
        actionKinds = $actionKinds
        actions = $actions
        actionRecordCount = $actionRecordCount
        retainedActionCount = $retainedActionCount
        unretainedActionCount = $unretainedActionCount
        actionsTruncated = $actionsTruncated
        closureProofAvailable = $closureProofAvailable
        workActionCount = $workActionCount
        unexpectedToolNames = $unexpectedToolNames
        unexpectedToolNamesTruncated = $unexpectedToolNamesTruncated
        logAvailable = $true
    }))
}

function Assert-PhaseSucceeded {
    param([Parameter(Mandatory)][object] $PhaseResult)
    if ($PhaseResult.result.type -ne 'ok') { return $PhaseResult.result }
    return $null
}

function Add-PhaseArtifactData {
    param(
        [Parameter(Mandatory)][object] $PhaseResult,
        [Parameter(Mandatory)][AllowEmptyCollection()][object[]] $Before,
        [Parameter(Mandatory)][object[]] $After
    )
    $PhaseResult | Add-Member -NotePropertyName artifactsBefore -NotePropertyValue $Before
    $PhaseResult | Add-Member -NotePropertyName artifactsAfter -NotePropertyValue $After
    $PhaseResult | Add-Member -NotePropertyName artifactChanges -NotePropertyValue @(Compare-ArtifactSnapshots -Before $Before -After $After)
    return $PhaseResult
}

function Get-NoOpViolation {
    param(
        [AllowNull()][AllowEmptyCollection()][object[]] $Actions = @(),
        [Parameter(Mandatory)][string] $Phase,
        [switch] $ActionsTruncated,
        [int] $AggregateWorkActionCount = -1,
        [int] $AggregateUnexpectedActionCount = -1
    )
    $workActions = @(Get-WorkActions $Actions)
    $unexpected = @($Actions | Where-Object { $_.kind -in $script:UnexpectedActionKinds })
    if ($workActions.Count -eq 0 -and $unexpected.Count -eq 0 -and -not $ActionsTruncated) { return $null }
    $workActionCount = if ($AggregateWorkActionCount -ge 0) { $AggregateWorkActionCount } else { [int]$workActions.Count }
    $unexpectedActionCount = if ($AggregateUnexpectedActionCount -ge 0) { $AggregateUnexpectedActionCount } else { [int]$unexpected.Count }
    return (New-OrderedObject ([ordered]@{
        type = 'unexpected_action'
        phase = $Phase
        workActionCount = $workActionCount
        unexpectedActionCount = $unexpectedActionCount
        actionsTruncated = [bool]$ActionsTruncated
    }))
}

function Append-WhitespaceMutation {
    param(
        [Parameter(Mandatory)][string] $Workspace,
        [Parameter(Mandatory)][string] $RelativePath
    )
    $path = [IO.Path]::GetFullPath((Join-Path $Workspace ($RelativePath.Replace('/', '\'))))
    if (-not (Test-PathBelow -Path $path -Root $Workspace) -or -not [IO.File]::Exists($path)) {
        throw "mutation path is missing or outside worktree: $RelativePath"
    }
    Assert-NoReparsePoint -Path $path -Context 'mutation path'
    $before = [IO.File]::ReadAllBytes($path)
    $after = New-Object byte[] ($before.Length + 1)
    [Array]::Copy($before, $after, $before.Length)
    $after[$before.Length] = 0x0A
    [IO.File]::WriteAllBytes($path, $after)
    $beforeHash = [Security.Cryptography.SHA256]::Create()
    try {
        $old = ([BitConverter]::ToString($beforeHash.ComputeHash($before))).Replace('-', '').ToLowerInvariant()
        $new = Get-Sha256 $path
    } finally { $beforeHash.Dispose() }
    if ($old -eq $new) { throw "mutation did not change bytes: $RelativePath" }
    return (New-OrderedObject ([ordered]@{
        relativePath = $RelativePath
        kind = 'trailing-whitespace'
        beforeSha256 = $old
        afterSha256 = $new
    }))
}

function Invoke-Git {
    param(
        [Parameter(Mandatory)][string] $WorkingDirectory,
        [Parameter(Mandatory)][string[]] $Arguments
    )
    $result = & git -C $WorkingDirectory @Arguments 2>&1
    if ($LASTEXITCODE -ne 0) { throw "git command failed: $($Arguments -join ' ')" }
    return @($result)
}

function Get-CheckoutFingerprint {
    param([Parameter(Mandatory)][string] $RepositoryRoot)
    $head = ((Invoke-Git -WorkingDirectory $RepositoryRoot -Arguments @('rev-parse', 'HEAD')) -join '').Trim()
    $statusLines = @(Invoke-Git -WorkingDirectory $RepositoryRoot -Arguments @('status', '--porcelain=v1', '--untracked-files=all'))
    $submoduleRows = [System.Collections.Generic.List[string]]::new()
    $initializedSubmoduleCount = 0
    $submoduleConfig = @(Invoke-Git -WorkingDirectory $RepositoryRoot -Arguments @(
        'config', '-f', (Join-Path $RepositoryRoot '.gitmodules'), '--get-regexp', '^submodule\..*\.path$'
    ))
    foreach ($line in @($submoduleConfig | Sort-Object)) {
        $match = [regex]::Match([string]$line, '^submodule\..*\.path\s+(?<path>.+)$')
        if (-not $match.Success) { throw 'submodule path configuration is malformed' }
        $relativePath = $match.Groups['path'].Value.Trim().Replace('\', '/')
        if ([string]::IsNullOrWhiteSpace($relativePath) -or $relativePath -match '[\r\n|]') {
            throw 'submodule path is not fingerprint-safe'
        }
        $treeLine = ((Invoke-Git -WorkingDirectory $RepositoryRoot -Arguments @('ls-tree', 'HEAD', '--', $relativePath)) -join '').Trim()
        $treeMatch = [regex]::Match($treeLine, '^160000\s+commit\s+(?<commit>[a-f0-9]{40})\s+')
        if (-not $treeMatch.Success) { throw 'submodule gitlink is malformed' }
        $expectedCommit = $treeMatch.Groups['commit'].Value
        $submoduleRoot = Get-FullPath (Join-Path $RepositoryRoot $relativePath)
        Assert-PathBelow -Path $submoduleRoot -Root $RepositoryRoot -Context 'submodule fingerprint path' | Out-Null
        $initialized = [IO.File]::Exists((Join-Path $submoduleRoot '.git')) -or
            [IO.Directory]::Exists((Join-Path $submoduleRoot '.git'))
        $submoduleHead = ''
        $submoduleStatus = @()
        $submoduleDiff = @()
        $submoduleLocalConfig = @()
        if ($initialized) {
            $initializedSubmoduleCount++
            $submoduleHead = ((Invoke-Git -WorkingDirectory $submoduleRoot -Arguments @('rev-parse', 'HEAD')) -join '').Trim()
            if ($submoduleHead -notmatch '^[a-f0-9]{40}$') { throw 'submodule HEAD is malformed' }
            $submoduleStatus = @(Invoke-Git -WorkingDirectory $submoduleRoot -Arguments @('status', '--porcelain=v1', '--untracked-files=all'))
            $submoduleDiff = @(Invoke-Git -WorkingDirectory $submoduleRoot -Arguments @('diff', '--binary'))
            $submoduleLocalConfig = @(Invoke-Git -WorkingDirectory $submoduleRoot -Arguments @('config', '--local', '--list'))
        }
        $submoduleRows.Add((@(
            $relativePath,
            $expectedCommit,
            ([string][bool]$initialized).ToLowerInvariant(),
            $submoduleHead,
            [string]$submoduleStatus.Count,
            (Get-StringSequenceSha256 $submoduleStatus),
            (Get-StringSequenceSha256 $submoduleDiff),
            (Get-StringSequenceSha256 $submoduleLocalConfig)
        ) -join '|')) | Out-Null
    }
    return (New-OrderedObject ([ordered]@{
        head = $head
        statusSha256 = Get-StringSequenceSha256 $statusLines
        statusLineCount = [int]$statusLines.Count
        submoduleCount = [int]$submoduleRows.Count
        initializedSubmoduleCount = [int]$initializedSubmoduleCount
        submoduleSha256 = Get-StringSequenceSha256 $submoduleRows.ToArray()
    }))
}

function Test-CheckoutFingerprintEqual {
    param(
        [Parameter(Mandatory)][object] $Left,
        [Parameter(Mandatory)][object] $Right
    )
    foreach ($name in @(
        'head', 'statusSha256', 'statusLineCount',
        'submoduleCount', 'initializedSubmoduleCount', 'submoduleSha256'
    )) {
        if ([string]$Left.$name -ne [string]$Right.$name) { return $false }
    }
    return $true
}

function Get-WorktreeRegistrationState {
    param(
        [Parameter(Mandatory)][string] $RepositoryRoot,
        [Parameter(Mandatory)][string] $Workspace,
        [Parameter(Mandatory)][AllowEmptyCollection()][AllowEmptyString()][string[]] $Lines
    )
    if ($Lines.Count -eq 0) { throw 'worktree registration output is empty' }
    $repository = Get-FullPath $RepositoryRoot
    $wanted = Get-FullPath $Workspace
    $repositoryRegistered = $false
    $wantedRegistered = $false
    $recordCount = 0
    foreach ($line in $Lines) {
        $text = [string]$line
        if ($text -match '^worktree(?:\s|$)' -and $text -notmatch '^worktree\s+(?<path>.+)$') {
            throw 'worktree registration output is malformed'
        }
        if ($text -match '^worktree\s+(?<path>.+)$') {
            $path = $Matches.path.Trim()
            if ([string]::IsNullOrWhiteSpace($path) -or -not [IO.Path]::IsPathRooted($path)) {
                throw 'worktree registration path is malformed'
            }
            $current = Get-FullPath $path
            $recordCount++
            if ($current.Equals($repository, [StringComparison]::OrdinalIgnoreCase)) {
                $repositoryRegistered = $true
            }
            if ($current.Equals($wanted, [StringComparison]::OrdinalIgnoreCase)) {
                $wantedRegistered = $true
            }
        }
    }
    if ($recordCount -eq 0 -or -not $repositoryRegistered) {
        throw 'worktree registration output omitted the invoking checkout'
    }
    return $wantedRegistered
}

function Test-WorktreeRegistration {
    param(
        [Parameter(Mandatory)][string] $RepositoryRoot,
        [Parameter(Mandatory)][string] $Workspace
    )
    $lines = @(Invoke-Git -WorkingDirectory $RepositoryRoot -Arguments @('worktree', 'list', '--porcelain'))
    return (Get-WorktreeRegistrationState -RepositoryRoot $RepositoryRoot -Workspace $Workspace -Lines $lines)
}

function New-IsolatedWorktree {
    param(
        [Parameter(Mandatory)][string] $RepositoryRoot,
        [Parameter(Mandatory)][string] $RequestedRoot,
        [Parameter(Mandatory)][int] $TimeoutSeconds,
        [Parameter(Mandatory)][ref] $SetupEvidence
    )
    $SetupEvidence.Value = New-OrderedObject ([ordered]@{
        stage = 'workspace_setup'
        result = (New-OrderedObject ([ordered]@{ type = 'process_error'; phase = 'workspace_setup' }))
    })
    $buildTmp = Join-Path $RepositoryRoot 'build/tmp'
    $base = if ([IO.Path]::IsPathRooted($RequestedRoot)) { Get-FullPath $RequestedRoot } else { Get-FullPath (Join-Path $RepositoryRoot $RequestedRoot) }
    Assert-PathBelow -Path $base -Root $buildTmp -Context 'WorkspaceRoot' -AllowRoot | Out-Null
    Assert-NoReparsePoint -Path $buildTmp -Context 'build/tmp'
    New-Item -ItemType Directory -Path $base -Force | Out-Null
    $name = "r-$PID-$([Guid]::NewGuid().ToString('N').Substring(0, 8))"
    $workspace = Join-Path $base $name
    Assert-PathBelow -Path $workspace -Root $base -Context 'owned workspace' | Out-Null
    if ([IO.Directory]::Exists($workspace) -or [IO.File]::Exists($workspace)) { throw "owned workspace already exists: $workspace" }
    New-Item -ItemType Directory -Path $base -Force | Out-Null
    try {
        $null = Invoke-Git -WorkingDirectory $RepositoryRoot -Arguments @('worktree', 'add', '--detach', $workspace, 'HEAD')
        if (-not [IO.Directory]::Exists($workspace)) { throw "git did not create worktree: $workspace" }
        # Re-check the newly created base and worktree.  A junction inserted
        # between validation and use must fail closed before any source read.
        Assert-NoReparsePoint -Path $base -Context 'workspace base'
        Assert-NoReparsePoint -Path $workspace -Context 'owned worktree'
        $head = (Invoke-Git -WorkingDirectory $workspace -Arguments @('rev-parse', 'HEAD')) -join ''
        $rootHead = (Invoke-Git -WorkingDirectory $RepositoryRoot -Arguments @('rev-parse', 'HEAD')) -join ''
        if ($head.Trim() -ne $rootHead.Trim()) { throw 'isolated worktree HEAD differs from shared checkout HEAD' }
        # Worktree submodules are initialized only inside the worktree.  Use the
        # same bounded process owner as build phases; an unbounded git fetch is
        # not acceptable in a bounded verifier.
        $logDirectory = Join-Path $workspace 'build/logs/native-rust-incremental'
        New-Item -ItemType Directory -Path $logDirectory -Force | Out-Null
        $submoduleStdOut = Join-Path $logDirectory 'submodule-update.stdout.log'
        $submoduleStdErr = Join-Path $logDirectory 'submodule-update.stderr.log'
        $submodule = Invoke-BoundedProcess -FilePath 'git.exe' `
            -ArgumentList @('submodule', 'update', '--init', '--recursive') `
            -WorkingDirectory $workspace `
            -StdOutPath $submoduleStdOut -StdErrPath $submoduleStdErr `
            -TimeoutSeconds $TimeoutSeconds -Phase 'submodule_update'
        $SetupEvidence.Value = New-OrderedObject ([ordered]@{
            stage = 'submodule_update'
            result = $submodule
        })
        try {
            $submodule = Add-ProcessOutputMetadata -Result $submodule `
                -StdOutPath $submoduleStdOut -StdErrPath $submoduleStdErr
            $SetupEvidence.Value = New-OrderedObject ([ordered]@{
                stage = 'submodule_update'
                result = $submodule
            })
        } catch { }
        if ($submodule.type -ne 'ok') { throw "typed:$($submodule.type):submodule_update" }
        Assert-NoReparsePoint -Path $workspace -Context 'initialized worktree'
        return (Get-FullPath $workspace)
    } catch {
        $message = [string]$_.Exception.Message
        # The caller cannot receive a path when this function fails.  Unregister
        # and remove this exact candidate here before rethrowing the typed cause.
        $cleanupFailure = $null
        $registration = $false
        try { $registration = Test-WorktreeRegistration -RepositoryRoot $RepositoryRoot -Workspace $workspace } catch { $cleanupFailure = $_ }
        if ([IO.Directory]::Exists($workspace) -or [IO.File]::Exists($workspace) -or $registration) {
            try {
                Remove-IsolatedWorktree -RepositoryRoot $RepositoryRoot -Workspace $workspace -BuildTmp $buildTmp
            } catch { $cleanupFailure = $_ }
        }
        if ($null -ne $cleanupFailure) {
            throw 'typed:survivor:setup_cleanup'
        }
        throw $message
    }
}

function Remove-DirectoryRecoverable {
    param([Parameter(Mandatory)][string] $Path)
    Assert-NoDirectoryReparsePointsBelow -Path $Path -Context 'cleanup directory'
    try {
        [IO.Directory]::Delete($Path, $true)
        return
    } catch [UnauthorizedAccessException] {
        # Git packfiles and generated outputs can inherit ReadOnly.  Clear only
        # entries below this exact owned workspace, then retry Directory.Delete.
        # Walk one level at a time so a junction inserted during cleanup cannot
        # make an AllDirectories enumeration recurse outside the owned tree.
        $pending = [System.Collections.Generic.Stack[string]]::new()
        $pending.Push((Get-FullPath $Path))
        $inspected = 0
        while ($pending.Count -gt 0) {
            $current = $pending.Pop()
            foreach ($entry in [IO.Directory]::EnumerateFileSystemEntries($current, '*', [IO.SearchOption]::TopDirectoryOnly)) {
                $inspected++
                if ($inspected -gt $script:MaxCleanupInspectionEntries) {
                    throw "cleanup inspection exceeded its bounded entry limit"
                }
                $attributes = [IO.File]::GetAttributes($entry)
                $isReparsePoint = ($attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0
                $isDirectory = ($attributes -band [IO.FileAttributes]::Directory) -ne 0
                if ($isReparsePoint -and $isDirectory) {
                    throw "cleanup encountered a reparse point: $entry"
                }
                if ($isReparsePoint) { continue }
                if (($attributes -band [IO.FileAttributes]::ReadOnly) -ne 0) {
                    [IO.File]::SetAttributes($entry, $attributes -band (-bnot [IO.FileAttributes]::ReadOnly))
                }
                if ($isDirectory) { $pending.Push($entry) }
            }
        }
        try {
            $rootAttributes = [IO.File]::GetAttributes($Path)
            if (($rootAttributes -band [IO.FileAttributes]::ReadOnly) -ne 0) {
                [IO.File]::SetAttributes($Path, $rootAttributes -band (-bnot [IO.FileAttributes]::ReadOnly))
            }
        } catch { }
        Assert-NoDirectoryReparsePointsBelow -Path $Path -Context 'cleanup directory retry'
        [IO.Directory]::Delete($Path, $true)
    }
}

function Remove-IsolatedWorktree {
    param(
        [Parameter(Mandatory)][string] $RepositoryRoot,
        [Parameter(Mandatory)][string] $Workspace,
        [Parameter(Mandatory)][string] $BuildTmp,
        [switch] $Keep
    )
    if ($Keep) {
        Assert-PathBelow -Path $Workspace -Root $BuildTmp -Context 'kept workspace' | Out-Null
        Assert-NoDirectoryReparsePointsBelow -Path $Workspace -Context 'kept workspace'
        return (New-OrderedObject ([ordered]@{ cleaned = $false; kept = $true; path = (Get-RelativePath $Workspace $RepositoryRoot) }))
    }
    Assert-PathBelow -Path $Workspace -Root $BuildTmp -Context 'cleanup workspace' | Out-Null
    Assert-NoDirectoryReparsePointsBelow -Path $Workspace -Context 'cleanup workspace'
    if ([IO.File]::Exists($Workspace) -and -not [IO.Directory]::Exists($Workspace)) {
        throw "owned worktree path is not a directory: $Workspace"
    }
    $removed = $false
    try {
        # Two --force flags are required when a worktree contains modified or
        # untracked generated files (including initialized submodules).
        $null = Invoke-Git -WorkingDirectory $RepositoryRoot -Arguments @('worktree', 'remove', '--force', '--force', $Workspace)
    } catch {
        # A linked worktree shares superproject submodule configuration with
        # the invoking checkout.  Never run `git submodule deinit` here: when
        # worktree removal has already unregistered the workspace but failed
        # to delete a long path, deinit can erase the shared checkout's
        # submodule working tree.  Re-check registration below and manually
        # delete only an already-unregistered owned workspace.
    }
    $removed = -not (Test-WorktreeRegistration -RepositoryRoot $RepositoryRoot -Workspace $Workspace)
    if ($removed -and [IO.Directory]::Exists($Workspace)) {
        Remove-DirectoryRecoverable -Path $Workspace
    }
    $pathAbsent = -not [IO.Directory]::Exists($Workspace) -and -not [IO.File]::Exists($Workspace)
    if (-not $removed -or -not $pathAbsent) {
        throw "owned worktree could not be cleaned: $Workspace"
    }
    return (New-OrderedObject ([ordered]@{ cleaned = $true; kept = $false; path = (Get-RelativePath $Workspace $RepositoryRoot) }))
}

function Assert-ProcessOutputMetadataSchema {
    param([Parameter(Mandatory)][object] $Metadata)
    foreach ($streamName in @('stdout', 'stderr')) {
        $streamProperty = $Metadata.PSObject.Properties[$streamName]
        if ($null -eq $streamProperty) { throw "process output metadata is missing $streamName" }
        $stream = $streamProperty.Value
        if ($stream.available -isnot [bool] -or $stream.parserFailed -isnot [bool]) {
            throw "process output $streamName availability/parser status is invalid"
        }
        if ($null -ne $stream.byteCount -and [UInt64]$stream.byteCount -lt 0) { throw "process output $streamName byte count is invalid" }
        if ($null -ne $stream.lineCount -and [UInt64]$stream.lineCount -lt 0) { throw "process output $streamName line count is invalid" }
        if ($null -ne $stream.sha256 -and [string]$stream.sha256 -notmatch '^[a-f0-9]{64}$') {
            throw "process output $streamName hash is invalid"
        }
    }
    foreach ($name in @('outputAvailable', 'buildErrorsTruncated', 'errorCodesTruncated', 'failureRecordsTruncated', 'parserFailed', 'versionMatched', 'versionProofAvailable')) {
        $property = $Metadata.PSObject.Properties[$name]
        if ($null -eq $property -or $property.Value -isnot [bool]) { throw "process output metadata boolean is invalid: $name" }
    }
    foreach ($name in @('buildErrors', 'errorCodes')) {
        $map = $Metadata.PSObject.Properties[$name]
        if ($null -eq $map) { throw "process output metadata map is missing: $name" }
        $properties = @($map.Value.PSObject.Properties)
        if ($properties.Count -gt $script:MaxProcessFailureCodes) { throw "process output metadata map is unbounded: $name" }
        foreach ($property in $properties) {
            if ([string]$property.Name -notmatch '^[A-Z][A-Z0-9_]{2,62}$' -or [int]$property.Value -lt 0) {
                throw "process output metadata code is invalid: $name"
            }
        }
    }
    foreach ($name in @('buildErrorRecordCount', 'errorCodeRecordCount', 'failureRecordCount')) {
        $property = $Metadata.PSObject.Properties[$name]
        if ($null -eq $property -or [int]$property.Value -lt 0 -or [int]$property.Value -gt ($script:MaxProcessFailureRecords * 2)) {
            throw "process output metadata record count is invalid: $name"
        }
    }
    if ([int]$Metadata.failureRecordCount -ne ([int]$Metadata.buildErrorRecordCount + [int]$Metadata.errorCodeRecordCount)) {
        throw 'process output metadata record counts are inconsistent'
    }
    return $true
}

function Assert-PayloadFreeFileMetadataSchema {
    param(
        [Parameter(Mandatory)][object] $Metadata,
        [Parameter(Mandatory)][string] $Context
    )
    if ($Metadata.exists -isnot [bool]) { throw "$Context existence flag is invalid" }
    if ($Metadata.exists) {
        if ($null -eq $Metadata.sizeBytes -or [UInt64]$Metadata.sizeBytes -lt 0) {
            throw "$Context size is invalid"
        }
        if ([string]$Metadata.sha256 -notmatch '^[a-f0-9]{64}$') {
            throw "$Context hash is invalid"
        }
    } elseif ($null -ne $Metadata.sizeBytes -or $null -ne $Metadata.sha256) {
        throw "$Context missing-file metadata is inconsistent"
    }
    return $true
}

function Assert-PackageToolSchema {
    param([Parameter(Mandatory)][object] $PackageTool)
    if ([string]$PackageTool.type -notin @('ok', 'build_failed')) {
        throw 'package tool result type is invalid'
    }
    if ([string]$PackageTool.validation -notin @('matched', 'failed')) {
        throw 'package tool validation state is invalid'
    }
    if ($null -ne $PackageTool.expectedReleaseTag -and
        [string]$PackageTool.expectedReleaseTag -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$') {
        throw 'package tool expected release tag is invalid'
    }
    if ($null -ne $PackageTool.reasonCode -and
        [string]$PackageTool.reasonCode -notmatch '^TOOL_VCPKG_[A-Z0-9_]{2,62}$') {
        throw 'package tool reason code is invalid'
    }
    Assert-PayloadFreeFileMetadataSchema -Metadata $PackageTool.source -Context 'package tool source' | Out-Null
    Assert-PayloadFreeFileMetadataSchema -Metadata $PackageTool.isolated -Context 'isolated package tool' | Out-Null
    Assert-PayloadFreeFileMetadataSchema -Metadata $PackageTool.disableMetricsMarker -Context 'package tool metrics marker' | Out-Null
    if ($null -ne $PackageTool.versionProbe -and $PackageTool.versionProbe.PSObject.Properties['outputMetadata']) {
        Assert-ProcessOutputMetadataSchema -Metadata $PackageTool.versionProbe.outputMetadata | Out-Null
    }
    if ($PackageTool.type -eq 'ok') {
        if ($PackageTool.validation -ne 'matched' -or $null -ne $PackageTool.reasonCode -or
            -not $PackageTool.source.exists -or -not $PackageTool.isolated.exists -or
            $PackageTool.source.sizeBytes -ne $PackageTool.isolated.sizeBytes -or
            $PackageTool.source.sha256 -ne $PackageTool.isolated.sha256 -or
            -not $PackageTool.disableMetricsMarker.exists -or $PackageTool.disableMetricsMarker.sizeBytes -ne 0 -or
            $null -eq $PackageTool.versionValidation -or
            $null -eq $PackageTool.versionProbe -or
            -not $PackageTool.versionProbe.PSObject.Properties['outputMetadata']) {
            throw 'successful package tool evidence is internally inconsistent'
        }
        $validation = $PackageTool.versionValidation
        $probe = $PackageTool.versionProbe
        $probeMetadata = $probe.outputMetadata
        if ([string]::IsNullOrWhiteSpace([string]$PackageTool.expectedReleaseTag) -or
            [string]$PackageTool.expectedReleaseTag -ne [string]$validation.expectedReleaseTag) {
            throw 'successful package tool release tag evidence is inconsistent'
        }
        if (-not $validation.reportedVersionMatched -or -not $validation.proofAvailable -or
            $validation.parserFailed -or $validation.recordsTruncated -or
            [string]$validation.probeType -ne [string]$probe.type -or
            [int]$validation.probeExitCode -ne [int]$probe.exitCode -or
            [string]$probe.type -ne 'ok' -or [int]$probe.exitCode -ne 0) {
            throw 'successful package tool version probe evidence is inconsistent'
        }
        if (-not $probeMetadata.outputAvailable -or -not $probeMetadata.versionMatched -or
            -not $probeMetadata.versionProofAvailable -or $probeMetadata.parserFailed -or
            $probeMetadata.failureRecordsTruncated -or -not $probeMetadata.stdout.available -or
            [UInt64]$probeMetadata.stdout.byteCount -le 0 -or
            [UInt64]$probeMetadata.stdout.lineCount -le 0) {
            throw 'successful package tool stdout proof is unavailable'
        }
    } elseif ($PackageTool.validation -ne 'failed' -or [string]::IsNullOrWhiteSpace([string]$PackageTool.reasonCode)) {
        throw 'failed package tool evidence is missing a typed reason'
    }
    return $true
}

function Assert-UnexpectedToolNamesSchema {
    param([Parameter(Mandatory)][object] $Phase)
    if (-not $Phase.PSObject.Properties['unexpectedToolNames']) { return $true }
    if (-not $Phase.PSObject.Properties['unexpectedToolNamesTruncated'] -or
        $Phase.unexpectedToolNamesTruncated -isnot [bool]) {
        throw 'unexpected tool-name truncation evidence is invalid'
    }
    $properties = @($Phase.unexpectedToolNames.PSObject.Properties)
    if ($properties.Count -gt $script:MaxUnexpectedToolNames) {
        throw 'unexpected tool-name evidence exceeds its bound'
    }
    [Int64]$recordedCount = 0
    foreach ($property in $properties) {
        $name = [string]$property.Name
        if ($name -notmatch '^[a-z0-9_.-]{1,64}\.exe$') {
            throw 'unexpected tool-name evidence is not a sanitized basename'
        }
        $count = [Int64]$property.Value
        if ($count -le 0) { throw 'unexpected tool-name count is invalid' }
        $recordedCount += $count
    }
    if ($Phase.PSObject.Properties['actionCounts'] -and
        $Phase.actionCounts.PSObject.Properties['unexpected_tool']) {
        $aggregateCount = [Int64]$Phase.actionCounts.unexpected_tool
        if ($recordedCount -gt $aggregateCount -or
            (-not [bool]$Phase.unexpectedToolNamesTruncated -and $recordedCount -ne $aggregateCount)) {
            throw 'unexpected tool-name evidence does not match the action count'
        }
    }
    return $true
}

function Assert-EvidenceCoreSchema {
    param([Parameter(Mandatory)][object] $Evidence)
    if ([int]$Evidence.schemaVersion -ne $script:SchemaVersion) { throw 'evidence schemaVersion is invalid' }
    if ($Evidence.payloadFree -isnot [bool] -or -not [bool]$Evidence.payloadFree) { throw 'evidence must be payload-free' }
    if ($Evidence.phaseOrder.Count -lt 7) { throw 'evidence phaseOrder is incomplete' }
    foreach ($phase in @($Evidence.phases)) {
        if ([string]::IsNullOrWhiteSpace([string]$phase.name)) { throw 'phase name is missing' }
        if ([string]$phase.result.type -notin @('ok', 'timeout', 'missing_output', 'unexpected_consumer', 'survivor', 'build_failed', 'unexpected_action', 'artifact_changed', 'process_error')) {
            throw "phase result type is not typed: $($phase.result.type)"
        }
        Assert-UnexpectedToolNamesSchema -Phase $phase | Out-Null
    }
}

function Assert-PayloadFreeEvidence {
    param([Parameter(Mandatory)][object] $Evidence)
    $json = $Evidence | ConvertTo-Json -Depth 30 -Compress
    if ($json -match '(?i)"(?:commandLine|arguments|lineText|payload|sourceText)"\s*:') {
        throw 'payload-bearing evidence property was emitted'
    }
    if ($json -match '(?i)"(?:stdout|stderr)"\s*:\s*"') { throw 'raw process output was emitted' }
    if ($json -match '(?i)"(?:processId|parentId|creationDate|parentCreationDate|ownershipProof)"\s*:') {
        throw 'raw process identity was emitted'
    }
}

function Copy-PayloadFreeEvidenceValue {
    param([AllowNull()][object] $Value)
    if ($null -eq $Value) { return $null }
    if ($Value -is [string] -or $Value -is [ValueType]) { return $Value }
    if ($Value -is [Collections.IDictionary]) {
        $copy = [ordered]@{}
        foreach ($key in $Value.Keys) {
            $name = [string]$key
            if ($name -match '^(?i)(commandLine|arguments|lineText|payload|sourceText|processId|parentId|creationDate|parentCreationDate|ownershipProof)$') {
                continue
            }
            $child = $Value[$key]
            if ($name -match '^(?i)(stdout|stderr)$' -and $child -is [string]) { continue }
            $copy[$name] = Copy-PayloadFreeEvidenceValue $child
        }
        return (New-OrderedObject $copy)
    }
    if ($Value -is [Collections.IEnumerable]) {
        $items = [System.Collections.Generic.List[object]]::new()
        foreach ($item in $Value) { $items.Add((Copy-PayloadFreeEvidenceValue $item)) | Out-Null }
        Write-Output -NoEnumerate $items.ToArray()
        return
    }
    $properties = @($Value.PSObject.Properties | Where-Object { $_.MemberType -in @('NoteProperty', 'Property') })
    if ($properties.Count -eq 0) { return $null }
    $copy = [ordered]@{}
    foreach ($property in $properties) {
        $name = [string]$property.Name
        if ($name -match '^(?i)(commandLine|arguments|lineText|payload|sourceText|processId|parentId|creationDate|parentCreationDate|ownershipProof)$') {
            continue
        }
        $child = $property.Value
        if ($name -match '^(?i)(stdout|stderr)$' -and $child -is [string]) { continue }
        $copy[$name] = Copy-PayloadFreeEvidenceValue $child
    }
    return (New-OrderedObject $copy)
}

function Get-SafeEvidenceStage {
    param([AllowNull()][string] $Stage)
    if ([string]::IsNullOrWhiteSpace($Stage) -or $Stage -notmatch '^[a-z0-9_]{1,64}$') {
        return 'schema_validation'
    }
    return $Stage
}

function New-SchemaFailureEvidence {
    param(
        [Parameter(Mandatory)][object] $Evidence,
        [Parameter(Mandatory)][string] $Code,
        [AllowNull()][string] $Stage
    )
    $safeStage = Get-SafeEvidenceStage $Stage
    $copy = Copy-PayloadFreeEvidenceValue $Evidence
    if ($null -eq $copy) { $copy = New-OrderedObject ([ordered]@{}) }
    $copy | Add-Member -NotePropertyName schemaVersion -NotePropertyValue $script:SchemaVersion -Force
    $copy | Add-Member -NotePropertyName verifier -NotePropertyValue $script:VerifierName -Force
    $copy | Add-Member -NotePropertyName payloadFree -NotePropertyValue $true -Force
    $copy | Add-Member -NotePropertyName status -NotePropertyValue 'failed' -Force
    if (-not $copy.PSObject.Properties['phaseOrder'] -or @($copy.phaseOrder).Count -lt 7) {
        $copy | Add-Member -NotePropertyName phaseOrder -NotePropertyValue @(
            'baseline', 'no_op_1', 'no_op_2', 'no_op_3', 'rust_source',
            'rust_output_provider', 'cpp_provider'
        ) -Force
    }
    if (-not $copy.PSObject.Properties['phases']) {
        $copy | Add-Member -NotePropertyName phases -NotePropertyValue @() -Force
    }
    $safePhases = [System.Collections.Generic.List[object]]::new()
    foreach ($phase in @($copy.phases)) {
        if ($null -eq $phase) { continue }
        $name = [string]$phase.name
        if ([string]::IsNullOrWhiteSpace($name)) { $name = 'schema_validation' }
        $phase | Add-Member -NotePropertyName name -NotePropertyValue $name -Force
        $resultType = [string]$phase.result.type
        if ($resultType -notin @(
            'ok', 'timeout', 'missing_output', 'unexpected_consumer', 'survivor', 'build_failed',
            'unexpected_action', 'artifact_changed', 'process_error'
        )) {
            $phase | Add-Member -NotePropertyName result -NotePropertyValue (
                New-OrderedObject ([ordered]@{ type = 'process_error'; phase = $safeStage })
            ) -Force
        }
        $safePhases.Add($phase) | Out-Null
    }
    $copy | Add-Member -NotePropertyName phases -NotePropertyValue $safePhases.ToArray() -Force
    if (-not $copy.PSObject.Properties['mutations']) {
        $copy | Add-Member -NotePropertyName mutations -NotePropertyValue @() -Force
    }
    if (-not $copy.PSObject.Properties['cleanup'] -or $null -eq $copy.cleanup) {
        $copy | Add-Member -NotePropertyName cleanup -NotePropertyValue (
            New-OrderedObject ([ordered]@{ workspaceCreated = $false; workspaceCleaned = $false; kept = $false; survivors = @() })
        ) -Force
    }
    $copy | Add-Member -NotePropertyName failure -NotePropertyValue (
        New-OrderedObject ([ordered]@{ type = 'process_error'; phase = $safeStage })
    ) -Force
    $copy | Add-Member -NotePropertyName schemaValidation -NotePropertyValue (
        New-OrderedObject ([ordered]@{ valid = $false; code = $Code; stage = $safeStage })
    ) -Force
    return $copy
}

function New-SafeEmergencyPhase {
    param([Parameter(Mandatory)][object] $Phase)
    $name = 'schema_validation'
    try {
        $candidateName = [string]$Phase.name
        if (-not [string]::IsNullOrWhiteSpace($candidateName) -and
            $candidateName -match '^[a-z0-9_]{1,64}$') { $name = $candidateName }
    } catch { }
    $allowedTypes = @(
        'ok', 'timeout', 'missing_output', 'unexpected_consumer', 'survivor', 'build_failed',
        'unexpected_action', 'artifact_changed', 'process_error'
    )
    $type = 'process_error'
    try {
        $candidateType = [string]$Phase.result.type
        if ($candidateType -in $allowedTypes) { $type = $candidateType }
    } catch { }
    $result = [ordered]@{ type = $type }
    foreach ($field in @(
        'exitCode', 'observedSurvivorCount', 'observedProcessCount',
        'observedExpectedHelperCount', 'postCleanupSurvivorCount'
    )) {
        try {
            if ($Phase.result.PSObject.Properties[$field]) {
                $value = $Phase.result.$field
                $result[$field] = if ($null -eq $value) { $null } else { [Int64]$value }
            }
        } catch { }
    }
    foreach ($field in @('timedOut', 'cleanupVerified', 'postCleanupKnown')) {
        try {
            if ($Phase.result.PSObject.Properties[$field]) { $result[$field] = [bool]$Phase.result.$field }
        } catch { }
    }
    try {
        if ($Phase.result.PSObject.Properties['durationSeconds']) {
            $duration = [double]$Phase.result.durationSeconds
            if ($duration -ge 0 -and -not [double]::IsNaN($duration) -and -not [double]::IsInfinity($duration)) {
                $result.durationSeconds = [Math]::Round($duration, 3)
            }
        }
    } catch { }
    try {
        $underlyingType = [string]$Phase.result.underlyingResultType
        if ($underlyingType -in $allowedTypes) { $result.underlyingResultType = $underlyingType }
    } catch { }
    try {
        $cleanup = [string]$Phase.result.cleanup
        if ($cleanup -in @('verified', 'survivor', 'unavailable', 'not_started')) { $result.cleanup = $cleanup }
    } catch { }

    $safePhase = [ordered]@{
        name = $name
        result = (New-OrderedObject $result)
    }
    try {
        if ($Phase.PSObject.Properties['diagnosticsParseFailed']) {
            $safePhase.diagnosticsParseFailed = [bool]$Phase.diagnosticsParseFailed
        }
    } catch { }
    foreach ($field in @(
        'workActionCount', 'actionRecordCount', 'retainedActionCount', 'unretainedActionCount'
    )) {
        try {
            if ($Phase.PSObject.Properties[$field]) { $safePhase[$field] = [Int64]$Phase.$field }
        } catch { }
    }
    try {
        if ($Phase.PSObject.Properties['actionsTruncated']) {
            $safePhase.actionsTruncated = [bool]$Phase.actionsTruncated
        }
    } catch { }
    try {
        if ($Phase.PSObject.Properties['actionCounts'] -and $null -ne $Phase.actionCounts) {
            $safeCounts = [ordered]@{}
            foreach ($kind in @($script:WorkActionKinds + $script:UnexpectedActionKinds)) {
                try {
                    $count = [Int64]$Phase.actionCounts.$kind
                    if ($count -ge 0) { $safeCounts[$kind] = $count }
                } catch { }
            }
            $safePhase.actionCounts = New-OrderedObject $safeCounts
        }
    } catch { }
    try {
        if ($Phase.PSObject.Properties['unexpectedToolNames'] -and $null -ne $Phase.unexpectedToolNames) {
            $safeNames = [ordered]@{}
            $namesTruncated = $false
            try {
                if ($Phase.PSObject.Properties['unexpectedToolNamesTruncated']) {
                    $namesTruncated = [bool]$Phase.unexpectedToolNamesTruncated
                }
            } catch { $namesTruncated = $true }
            $properties = @($Phase.unexpectedToolNames.PSObject.Properties)
            if ($properties.Count -gt $script:MaxUnexpectedToolNames) { $namesTruncated = $true }
            foreach ($property in @($properties | Select-Object -First $script:MaxUnexpectedToolNames)) {
                $name = [string]$property.Name
                if ($name -notmatch '^[a-z0-9_.-]{1,64}\.exe$') {
                    $namesTruncated = $true
                    continue
                }
                try {
                    $count = [Int64]$property.Value
                    if ($count -gt 0) { $safeNames[$name] = $count } else { $namesTruncated = $true }
                } catch { $namesTruncated = $true }
            }
            if ($safePhase.Contains('actionCounts') -and
                $safePhase.actionCounts.PSObject.Properties['unexpected_tool']) {
                [Int64]$safeNameCount = 0
                foreach ($count in $safeNames.Values) { $safeNameCount += [Int64]$count }
                if ($safeNameCount -ne [Int64]$safePhase.actionCounts.unexpected_tool) {
                    $namesTruncated = $true
                }
            }
            $safePhase.unexpectedToolNames = New-OrderedObject $safeNames
            $safePhase.unexpectedToolNamesTruncated = [bool]$namesTruncated
        }
    } catch { }
    try {
        if ($Phase.PSObject.Properties['diagnostics'] -and $null -ne $Phase.diagnostics) {
            $safeDiagnostics = [ordered]@{}
            foreach ($field in @('available', 'errorCodesTruncated')) {
                try {
                    if ($Phase.diagnostics.PSObject.Properties[$field]) {
                        $safeDiagnostics[$field] = [bool]$Phase.diagnostics.$field
                    }
                } catch { }
            }
            foreach ($field in @('byteCount', 'lineCount')) {
                try {
                    if ($Phase.diagnostics.PSObject.Properties[$field]) {
                        $count = [UInt64]$Phase.diagnostics.$field
                        $safeDiagnostics[$field] = $count
                    }
                } catch { }
            }
            try {
                $sha256 = [string]$Phase.diagnostics.sha256
                if ($sha256 -match '^[a-f0-9]{64}$') { $safeDiagnostics.sha256 = $sha256 }
            } catch { }
            $safeErrorCodes = [ordered]@{}
            try {
                foreach ($property in @($Phase.diagnostics.errorCodes.PSObject.Properties | Select-Object -First 32)) {
                    $code = [string]$property.Name
                    if ($code -notmatch '^[A-Z][A-Z0-9]{1,15}$') { continue }
                    try {
                        $count = [Int64]$property.Value
                        if ($count -ge 0) { $safeErrorCodes[$code] = $count }
                    } catch { }
                }
            } catch { }
            $safeDiagnostics.errorCodes = New-OrderedObject $safeErrorCodes
            $safePhase.diagnostics = New-OrderedObject $safeDiagnostics
        }
    } catch { }
    return (New-OrderedObject $safePhase)
}

function New-EmergencyEvidenceEnvelope {
    param(
        [AllowNull()][object] $Evidence,
        [AllowNull()][string] $Code,
        [AllowNull()][string] $Stage
    )
    $safeStage = Get-SafeEvidenceStage $Stage
    $allowedCodes = @(
        'EVIDENCE_SCHEMA_CORE', 'EVIDENCE_SCHEMA_PROCESS_METADATA',
        'EVIDENCE_SCHEMA_PACKAGE_TOOL', 'EVIDENCE_SCHEMA_RAW_IDENTITY',
        'EVIDENCE_SCHEMA_SHARED_CHECKOUT'
    )
    $safeCode = if ($Code -in $allowedCodes) { $Code } else { 'EVIDENCE_SCHEMA_CORE' }
    $phases = [System.Collections.Generic.List[object]]::new()
    $sourcePhases = @()
    try {
        if ($null -ne $Evidence -and $Evidence.PSObject.Properties['phases']) {
            $sourcePhases = @($Evidence.phases)
        }
    } catch { }
    foreach ($phase in $sourcePhases) {
        if ($null -eq $phase) { continue }
        $phases.Add((New-SafeEmergencyPhase -Phase $phase)) | Out-Null
    }
    $workspaceCreated = $false
    $workspaceCleaned = $false
    $kept = $false
    if ($null -ne $Evidence -and $Evidence.PSObject.Properties['cleanup'] -and $null -ne $Evidence.cleanup) {
        $workspaceCreated = [bool]$Evidence.cleanup.workspaceCreated
        $workspaceCleaned = [bool]$Evidence.cleanup.workspaceCleaned
        $kept = [bool]$Evidence.cleanup.kept
    }
    return (New-OrderedObject ([ordered]@{
        schemaVersion = $script:SchemaVersion
        verifier = $script:VerifierName
        payloadFree = $true
        status = 'failed'
        phaseOrder = @(
            'baseline', 'no_op_1', 'no_op_2', 'no_op_3', 'rust_source',
            'rust_output_provider', 'cpp_provider'
        )
        phases = $phases.ToArray()
        mutations = @()
        cleanup = (New-OrderedObject ([ordered]@{
            workspaceCreated = $workspaceCreated
            workspaceCleaned = $workspaceCleaned
            kept = $kept
            survivors = @()
            survivorCount = if ($null -ne $Evidence -and $Evidence.PSObject.Properties['cleanup'] -and
                $null -ne $Evidence.cleanup -and $Evidence.cleanup.PSObject.Properties['survivors']) {
                [int]@($Evidence.cleanup.survivors).Count
            } else { 0 }
        }))
        failure = (New-OrderedObject ([ordered]@{ type = 'process_error'; phase = $safeStage }))
        schemaValidation = (New-OrderedObject ([ordered]@{
            valid = $false
            code = $safeCode
            stage = $safeStage
            retention = 'bounded_phase_metadata'
        }))
    }))
}

function Get-EvidenceSchemaValidation {
    param(
        [Parameter(Mandatory)][object] $Evidence,
        [AllowNull()][string] $Stage
    )
    $safeStage = Get-SafeEvidenceStage $Stage
    if ($Evidence.PSObject.Properties['schemaValidation'] -and
        $null -ne $Evidence.schemaValidation -and
        $Evidence.schemaValidation.valid -eq $false) {
        try {
            Assert-EvidenceCoreSchema -Evidence $Evidence
            if ($Evidence.status -ne 'failed' -or $Evidence.failure.type -ne 'process_error') {
                throw 'schema failure envelope is not failed closed'
            }
            if ([string]$Evidence.schemaValidation.code -notin @(
                'EVIDENCE_SCHEMA_CORE', 'EVIDENCE_SCHEMA_PROCESS_METADATA',
                'EVIDENCE_SCHEMA_PACKAGE_TOOL', 'EVIDENCE_SCHEMA_RAW_IDENTITY',
                'EVIDENCE_SCHEMA_SHARED_CHECKOUT'
            )) { throw 'schema failure envelope code is invalid' }
            if ([string]$Evidence.schemaValidation.stage -notmatch '^[a-z0-9_]{1,64}$') {
                throw 'schema failure envelope stage is invalid'
            }
            Assert-PayloadFreeEvidence -Evidence $Evidence
            return (New-OrderedObject ([ordered]@{ valid = $true; code = $null; stage = $safeStage }))
        } catch {
            return (New-OrderedObject ([ordered]@{ valid = $false; code = 'EVIDENCE_SCHEMA_CORE'; stage = $safeStage }))
        }
    }
    try {
        Assert-EvidenceCoreSchema -Evidence $Evidence
    } catch {
        return (New-OrderedObject ([ordered]@{ valid = $false; code = 'EVIDENCE_SCHEMA_CORE'; stage = $safeStage }))
    }
    try {
        if ($Evidence.PSObject.Properties['packageRestore'] -and $null -ne $Evidence.packageRestore -and
            $Evidence.packageRestore.PSObject.Properties['outputMetadata']) {
            Assert-ProcessOutputMetadataSchema -Metadata $Evidence.packageRestore.outputMetadata | Out-Null
        }
        if ($Evidence.PSObject.Properties['packageTool'] -and $null -ne $Evidence.packageTool -and
            $Evidence.packageTool.PSObject.Properties['versionProbe'] -and $null -ne $Evidence.packageTool.versionProbe -and
            $Evidence.packageTool.versionProbe.PSObject.Properties['outputMetadata']) {
            Assert-ProcessOutputMetadataSchema -Metadata $Evidence.packageTool.versionProbe.outputMetadata | Out-Null
        }
    } catch {
        return (New-OrderedObject ([ordered]@{ valid = $false; code = 'EVIDENCE_SCHEMA_PROCESS_METADATA'; stage = $safeStage }))
    }
    try {
        if ($Evidence.PSObject.Properties['packageTool'] -and $null -ne $Evidence.packageTool) {
            Assert-PackageToolSchema -PackageTool $Evidence.packageTool | Out-Null
        }
    } catch {
        return (New-OrderedObject ([ordered]@{ valid = $false; code = 'EVIDENCE_SCHEMA_PACKAGE_TOOL'; stage = $safeStage }))
    }
    try {
        Assert-PayloadFreeEvidence -Evidence $Evidence
    } catch {
        return (New-OrderedObject ([ordered]@{ valid = $false; code = 'EVIDENCE_SCHEMA_RAW_IDENTITY'; stage = $safeStage }))
    }
    try {
        if ($Evidence.PSObject.Properties['sharedCheckoutBefore'] -and
            $Evidence.PSObject.Properties['sharedCheckoutAfter']) {
            $sharedCheckoutChanged = -not (Test-CheckoutFingerprintEqual `
                -Left $Evidence.sharedCheckoutBefore -Right $Evidence.sharedCheckoutAfter)
            if ($sharedCheckoutChanged -and
                ($Evidence.status -ne 'failed' -or $Evidence.failure.type -ne 'artifact_changed' -or
                 $Evidence.failure.phase -ne 'shared_checkout_audit')) {
                throw 'shared checkout fingerprint mismatch is not typed'
            }
        }
    } catch {
        return (New-OrderedObject ([ordered]@{ valid = $false; code = 'EVIDENCE_SCHEMA_SHARED_CHECKOUT'; stage = $safeStage }))
    }
    return (New-OrderedObject ([ordered]@{ valid = $true; code = $null; stage = $safeStage }))
}

function Assert-EvidenceSchema {
    param([Parameter(Mandatory)][object] $Evidence)
    $validation = Get-EvidenceSchemaValidation -Evidence $Evidence -Stage 'schema_validation'
    if (-not $validation.valid) { throw "evidence schema validation failed: $($validation.code)" }
    return $true
}

function New-BaseEvidence {
    param(
        [Parameter(Mandatory)][string] $RepositoryRoot,
        [Parameter(Mandatory)][string] $Platform,
        [Parameter(Mandatory)][string] $Configuration,
        [Parameter(Mandatory)][int] $NoOpIterations,
        [Parameter(Mandatory)][int] $TimeoutSeconds
    )
    return (New-OrderedObject ([ordered]@{
        schemaVersion = $script:SchemaVersion
        verifier = $script:VerifierName
        payloadFree = $true
        status = 'running'
        configuration = (New-OrderedObject ([ordered]@{
            platform = $Platform
            configuration = $Configuration
            noOpIterations = $NoOpIterations
            timeoutSeconds = $TimeoutSeconds
            rustBuildTarget = $script:RustBuildTarget
            sourceCommit = ((Invoke-Git -WorkingDirectory $RepositoryRoot -Arguments @('rev-parse', 'HEAD')) -join '').Trim()
            environment = (New-OrderedObject ([ordered]@{
                SAKURA_OUTPUT_BACKEND = 'cpp'
                SAKURA_UTF16_BACKEND = 'cpp'
                SAKURA_OUTPUT_PRODUCTION_PACKAGE = 'false'
                SAKURA_UTF16_PRODUCTION_PACKAGE = 'false'
                SAKURA_UTF16_BENCHMARK_TELEMETRY = 'false'
                SKIP_CREATE_GITHASH = '1'
                MSBUILDDISABLENODEREUSE = '1'
                SAKURA_BUILD_JOBS = '1'
                VSLANG = '1033'
                CARGO_TERM_COLOR = 'never'
            }))
        }))
        sharedCheckoutBefore = Get-CheckoutFingerprint -RepositoryRoot $RepositoryRoot
        explicitExpectedConsumers = $script:ExpectedLinkConsumers
        phaseOrder = @('baseline') + @(1..$NoOpIterations | ForEach-Object { "no_op_$($_)" }) + @('rust_source', 'rust_output_provider', 'cpp_provider')
        phases = @()
        mutations = @()
        packageTool = $null
        packageRestore = $null
        workspaceSetup = $null
        cleanup = (New-OrderedObject ([ordered]@{ workspaceCreated = $false; workspaceCleaned = $false; kept = $false; survivors = @() }))
        failure = $null
    }))
}

function Invoke-Verifier {
    param(
        [Parameter(Mandatory)][string] $RepositoryRoot,
        [Parameter(Mandatory)][string] $Platform,
        [Parameter(Mandatory)][string] $Configuration,
        [Parameter(Mandatory)][int] $NoOpIterations,
        [Parameter(Mandatory)][int] $TimeoutSeconds,
        [Parameter(Mandatory)][string] $WorkspaceRoot,
        [switch] $KeepWorkspace
    )
    $evidence = New-BaseEvidence -RepositoryRoot $RepositoryRoot -Platform $Platform -Configuration $Configuration `
        -NoOpIterations $NoOpIterations -TimeoutSeconds $TimeoutSeconds
    $workspace = $null
    $workspaceSetup = $null
    $buildTmp = Join-Path $RepositoryRoot 'build/tmp'
    $lastStage = 'initialization'
    $savedEnvironment = [ordered]@{}
    foreach ($name in @(
        'SAKURA_OUTPUT_BACKEND', 'SAKURA_UTF16_BACKEND',
        'SAKURA_OUTPUT_PRODUCTION_PACKAGE', 'SAKURA_UTF16_PRODUCTION_PACKAGE',
        'SAKURA_UTF16_BENCHMARK_TELEMETRY', 'SKIP_CREATE_GITHASH',
        'MSBUILDDISABLENODEREUSE', 'SAKURA_BUILD_JOBS', 'VSLANG', 'CARGO_TERM_COLOR', 'VCPKG_ROOT'
    )) {
        $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
    }
    try {
        # Pin every backend/package switch in the child process environment.
        # This makes evidence independent of a developer's ambient candidate
        # selection and preserves the cargo-free validation contract.
        $env:SAKURA_OUTPUT_BACKEND = 'cpp'
        $env:SAKURA_UTF16_BACKEND = 'cpp'
        $env:SAKURA_OUTPUT_PRODUCTION_PACKAGE = 'false'
        $env:SAKURA_UTF16_PRODUCTION_PACKAGE = 'false'
        $env:SAKURA_UTF16_BENCHMARK_TELEMETRY = 'false'
        $env:SKIP_CREATE_GITHASH = '1'
        $env:MSBUILDDISABLENODEREUSE = '1'
        $env:SAKURA_BUILD_JOBS = '1'
        $env:VSLANG = '1033'
        $env:CARGO_TERM_COLOR = 'never'

        $lastStage = 'workspace_setup'
        $workspace = New-IsolatedWorktree -RepositoryRoot $RepositoryRoot -RequestedRoot $WorkspaceRoot `
            -TimeoutSeconds $TimeoutSeconds -SetupEvidence ([ref]$workspaceSetup)
        $evidence.workspaceSetup = $workspaceSetup
        $evidence.cleanup.workspaceCreated = $true
        $artifacts = Get-ArtifactDefinitions -Platform $Platform -Configuration $Configuration
        $lastStage = 'package_tool'
        $packageTool = Prepare-IsolatedVcpkgTool -RepositoryRoot $RepositoryRoot -Workspace $workspace -TimeoutSeconds $TimeoutSeconds
        $evidence | Add-Member -NotePropertyName packageTool -NotePropertyValue $packageTool -Force
        if ($packageTool.type -ne 'ok') { throw "typed:$($packageTool.type):package_tool" }
        # package_restore.py gives VCPKG_ROOT precedence over its context
        # manifest.  Pin that process-only selector to the owned worktree so
        # an ambient developer value cannot select another vcpkg installation.
        $env:VCPKG_ROOT = Get-IsolatedVcpkgRoot -Workspace $workspace
        $lastStage = 'package_restore'
        $packageRestore = Invoke-PackageRestore -Workspace $workspace -Configuration $Configuration -TimeoutSeconds $TimeoutSeconds
        $evidence | Add-Member -NotePropertyName packageRestore -NotePropertyValue $packageRestore -Force
        if ($packageRestore.type -ne 'ok') { throw "typed:$($packageRestore.type):package_restore" }
        $lastStage = 'baseline'
        $baseline = Invoke-MsbuildPhase -Workspace $workspace -Phase 'baseline' -Platform $Platform -Configuration $Configuration -TimeoutSeconds $TimeoutSeconds
        $evidence.phases += @($baseline)
        $baselineSnapshot = Get-ArtifactSnapshot -Workspace $workspace -Artifacts $artifacts
        $required = Assert-RequiredArtifacts -Snapshot $baselineSnapshot -Phase 'baseline'
        if ($baseline.result.type -ne 'ok') { $required = $baseline.result }
        $baseline = Add-PhaseArtifactData -PhaseResult $baseline -Before @() -After $baselineSnapshot
        $baseline | Add-Member -NotePropertyName closure -NotePropertyValue (Get-ExplicitConsumerClosure -Actions $baseline.actions -ExpectedConsumers $script:ExpectedLinkConsumers.baseline -Phase 'baseline' -ActionsTruncated:$baseline.actionsTruncated)
        if ($required.type -ne 'ok') { throw "typed:$($required.type):baseline" }
        if ($baseline.actionsTruncated -or [int]$baseline.actionCounts.'cargo-preflight' -ne 0 -or
            [int]$baseline.actionCounts.unexpected_tool -ne 0) {
            throw 'typed:unexpected_action:baseline:unclassified-tool'
        }
        if ($baseline.closure.type -ne 'ok') { throw 'typed:unexpected_consumer:baseline' }

        for ($index = 1; $index -le $NoOpIterations; $index++) {
            $phaseName = "no_op_$index"
            $lastStage = $phaseName
            $before = Get-ArtifactSnapshot -Workspace $workspace -Artifacts $artifacts
            $phase = Invoke-MsbuildPhase -Workspace $workspace -Phase $phaseName -Platform $Platform -Configuration $Configuration -TimeoutSeconds $TimeoutSeconds
            $evidence.phases += @($phase)
            $after = Get-ArtifactSnapshot -Workspace $workspace -Artifacts $artifacts
            $phase = Add-PhaseArtifactData -PhaseResult $phase -Before $before -After $after
            $phase | Add-Member -NotePropertyName closure -NotePropertyValue (Get-ExplicitConsumerClosure -Actions $phase.actions -ExpectedConsumers @() -Phase $phaseName -ActionsTruncated:$phase.actionsTruncated)
            if ($phase.result.type -ne 'ok') { throw "typed:$($phase.result.type):$phaseName" }
            $unexpectedCount = [int]$phase.actionCounts.'cargo-preflight' + [int]$phase.actionCounts.unexpected_tool
            $noOpViolation = Get-NoOpViolation -Actions $phase.actions -Phase $phaseName `
                -ActionsTruncated:$phase.actionsTruncated -AggregateWorkActionCount $phase.workActionCount `
                -AggregateUnexpectedActionCount $unexpectedCount
            if ($null -ne $noOpViolation) {
                $phase.result = $noOpViolation
                throw "typed:unexpected_action:$phaseName"
            }
            if ($phase.artifactChanges.Count -ne 0) {
                $phase.result = New-OrderedObject ([ordered]@{ type = 'artifact_changed'; phase = $phaseName; changes = $phase.artifactChanges })
                throw "typed:artifact_changed:$phaseName"
            }
        }

        $lastStage = 'rust_source'
        $rustMutation = Append-WhitespaceMutation -Workspace $workspace -RelativePath 'rust/native/sakura_native_ffi/src/lib.rs'
        $evidence.mutations += @($rustMutation)
        $rustBefore = Get-ArtifactSnapshot -Workspace $workspace -Artifacts $artifacts
        $rustPhase = Invoke-MsbuildPhase -Workspace $workspace -Phase 'rust_source' -Platform $Platform -Configuration $Configuration -TimeoutSeconds $TimeoutSeconds
        $evidence.phases += @($rustPhase)
        $rustAfter = Get-ArtifactSnapshot -Workspace $workspace -Artifacts $artifacts
        $rustPhase = Add-PhaseArtifactData -PhaseResult $rustPhase -Before $rustBefore -After $rustAfter
        $rustClosure = Get-ExplicitConsumerClosure -Actions $rustPhase.actions -ExpectedConsumers $script:ExpectedLinkConsumers.rust_source -Phase 'rust_source' -ActionsTruncated:$rustPhase.actionsTruncated
        $rustPhase | Add-Member -NotePropertyName closure -NotePropertyValue $rustClosure
        if ($rustPhase.result.type -ne 'ok') { throw "typed:$($rustPhase.result.type):rust_source" }
        if ($rustPhase.actionsTruncated) { throw 'typed:unexpected_action:rust_source:actions-truncated' }
        # A product relink invokes mt.exe to embed the generated manifest. It is
        # a typed link companion, not an unknown tool or a C++ recompilation.
        $rustForbidden = @($rustPhase.actions | Where-Object {
            $_.kind -in @('cl', 'lib', 'rc', 'cmake', 'senp-tool', 'delete', 'cargo-preflight', 'unexpected_tool')
        })
        if ($rustForbidden.Count -ne 0) { throw 'typed:unexpected_action:rust_source:forbidden-tool' }
        if (@($rustPhase.actions | Where-Object { $_.kind -eq 'cargo' -and $_.operation -eq 'build' }).Count -eq 0) { throw 'typed:unexpected_action:rust_source:no-cargo-build' }
        if ($rustClosure.type -ne 'ok') { throw 'typed:unexpected_consumer:rust_source' }
        $rustArchiveChanged = @($rustPhase.artifactChanges | Where-Object { $_.label -eq 'rust_archive' }).Count -gt 0
        $rustStampChanged = @($rustPhase.artifactChanges | Where-Object { $_.label -eq 'rust_stamp' }).Count -gt 0
        if (-not ($rustArchiveChanged -or $rustStampChanged)) { throw 'typed:artifact_changed:rust_source:no-rust-output-change' }

        $lastStage = 'rust_output_provider'
        $rustOutputProviderMutation = Append-WhitespaceMutation -Workspace $workspace `
            -RelativePath 'rust/native/sakura_native_ffi/src/output_provider.rs'
        $evidence.mutations += @($rustOutputProviderMutation)
        $rustOutputProviderBefore = Get-ArtifactSnapshot -Workspace $workspace -Artifacts $artifacts
        $rustOutputProviderPhase = Invoke-MsbuildPhase -Workspace $workspace -Phase 'rust_output_provider' `
            -Platform $Platform -Configuration $Configuration -TimeoutSeconds $TimeoutSeconds
        $evidence.phases += @($rustOutputProviderPhase)
        $rustOutputProviderAfter = Get-ArtifactSnapshot -Workspace $workspace -Artifacts $artifacts
        $rustOutputProviderPhase = Add-PhaseArtifactData -PhaseResult $rustOutputProviderPhase `
            -Before $rustOutputProviderBefore -After $rustOutputProviderAfter
        $rustOutputProviderClosure = Get-ExplicitConsumerClosure -Actions $rustOutputProviderPhase.actions `
            -ExpectedConsumers $script:ExpectedLinkConsumers.rust_output_provider `
            -Phase 'rust_output_provider' -ActionsTruncated:$rustOutputProviderPhase.actionsTruncated
        $rustOutputProviderPhase | Add-Member -NotePropertyName closure `
            -NotePropertyValue $rustOutputProviderClosure
        if ($rustOutputProviderPhase.result.type -ne 'ok') {
            throw "typed:$($rustOutputProviderPhase.result.type):rust_output_provider"
        }
        if ($rustOutputProviderPhase.actionsTruncated) {
            throw 'typed:unexpected_action:rust_output_provider:actions-truncated'
        }
        $rustOutputProviderForbidden = @($rustOutputProviderPhase.actions | Where-Object {
            $_.kind -in @('cl', 'lib', 'rc', 'cmake', 'senp-tool', 'delete',
                'cargo-preflight', 'unexpected_tool')
        })
        if ($rustOutputProviderForbidden.Count -ne 0) {
            throw 'typed:unexpected_action:rust_output_provider:forbidden-tool'
        }
        if (@($rustOutputProviderPhase.actions | Where-Object {
            $_.kind -eq 'cargo' -and $_.operation -eq 'build'
        }).Count -eq 0) {
            throw 'typed:unexpected_action:rust_output_provider:no-cargo-build'
        }
        $rustOutputProviderLinks = @($rustOutputProviderPhase.actions | Where-Object {
            $_.kind -eq 'link'
        })
        if ($rustOutputProviderLinks.Count -ne 1) {
            throw 'typed:unexpected_consumer:rust_output_provider:link-count'
        }
        if ($rustOutputProviderClosure.type -ne 'ok') {
            throw 'typed:unexpected_consumer:rust_output_provider'
        }
        $rustOutputProviderArchiveChanged = @($rustOutputProviderPhase.artifactChanges |
            Where-Object { $_.label -eq 'rust_archive' }).Count -gt 0
        $rustOutputProviderStampChanged = @($rustOutputProviderPhase.artifactChanges |
            Where-Object { $_.label -eq 'rust_stamp' }).Count -gt 0
        if (-not $rustOutputProviderArchiveChanged) {
            throw 'typed:artifact_changed:rust_output_provider:no-rust-archive-change'
        }
        if (-not $rustOutputProviderStampChanged) {
            throw 'typed:artifact_changed:rust_output_provider:no-rust-stamp-change'
        }

        $lastStage = 'cpp_provider'
        $cppMutation = Append-WhitespaceMutation -Workspace $workspace -RelativePath 'sakura_core/workbench/output/OutputServiceRustProvider.cpp'
        $evidence.mutations += @($cppMutation)
        $cppBefore = Get-ArtifactSnapshot -Workspace $workspace -Artifacts $artifacts
        $cppPhase = Invoke-MsbuildPhase -Workspace $workspace -Phase 'cpp_provider' -Platform $Platform -Configuration $Configuration -TimeoutSeconds $TimeoutSeconds
        $evidence.phases += @($cppPhase)
        $cppAfter = Get-ArtifactSnapshot -Workspace $workspace -Artifacts $artifacts
        $cppPhase = Add-PhaseArtifactData -PhaseResult $cppPhase -Before $cppBefore -After $cppAfter
        $cppClosure = Get-ExplicitConsumerClosure -Actions $cppPhase.actions -ExpectedConsumers $script:ExpectedLinkConsumers.cpp_provider -Phase 'cpp_provider' -ActionsTruncated:$cppPhase.actionsTruncated
        $cppPhase | Add-Member -NotePropertyName closure -NotePropertyValue $cppClosure
        if ($cppPhase.result.type -ne 'ok') { throw "typed:$($cppPhase.result.type):cpp_provider" }
        if ($cppPhase.actionsTruncated) { throw 'typed:unexpected_action:cpp_provider:actions-truncated' }
        $cppForbidden = @($cppPhase.actions | Where-Object {
            $_.kind -in @('cargo', 'cargo-preflight', 'rustc', 'lib', 'rc', 'cmake', 'senp-tool', 'delete', 'unexpected_tool')
        })
        if ($cppForbidden.Count -ne 0) { throw 'typed:unexpected_action:cpp_provider:forbidden-tool' }
        $providerCompiles = @($cppPhase.actions | Where-Object {
            $_.kind -eq 'cl' -and @($_.sourcePaths | Where-Object { $_ -match '(?i)(^|/)sakura_core/workbench/output/OutputServiceRustProvider\.cpp$' }).Count -gt 0
        })
        if ($providerCompiles.Count -ne 1) { throw 'typed:unexpected_action:cpp_provider:provider-compile-count' }
        $otherCompiles = @($cppPhase.actions | Where-Object { $_.kind -eq 'cl' -and $_ -notin $providerCompiles })
        if ($otherCompiles.Count -ne 0) { throw 'typed:unexpected_action:cpp_provider:other-cpp-compile' }
        if ($cppClosure.type -ne 'ok') { throw 'typed:unexpected_consumer:cpp_provider' }
        if (@($cppPhase.artifactChanges | Where-Object { $_.label -eq 'provider_obj' }).Count -eq 0) { throw 'typed:artifact_changed:cpp_provider:no-provider-object-change' }

        $lastStage = 'complete'
        $evidence.status = 'passed'
        $evidence.failure = $null
    } catch {
        if ($null -ne $workspaceSetup) { $evidence.workspaceSetup = $workspaceSetup }
        $message = [string]$_.Exception.Message
        $typedMatch = [regex]::Match($message, '^typed:(?<type>[^:]+):(?<phase>[^:]+)')
        if ($typedMatch.Success) {
            $type = $typedMatch.Groups['type'].Value
            $phase = $typedMatch.Groups['phase'].Value
        } else {
            $type = 'build_failed'
            $phase = Get-SafeEvidenceStage $lastStage
        }
        $evidence.status = 'failed'
        $evidence.failure = New-OrderedObject ([ordered]@{
            type = $type
            phase = $phase
        })
    } finally {
        if ($workspace) {
            try {
                $cleanup = Remove-IsolatedWorktree -RepositoryRoot $RepositoryRoot -Workspace $workspace -BuildTmp $buildTmp -Keep:$KeepWorkspace
                $evidence.cleanup.workspaceCleaned = [bool]$cleanup.cleaned
                $evidence.cleanup.kept = [bool]$cleanup.kept
                # Keep the workspace location out of the payload-free evidence.
                # Its exact path remains discoverable from `git worktree list`.
            } catch {
                $evidence.status = 'failed'
                $evidence.cleanup.workspaceCleaned = $false
                $evidence.failure = New-OrderedObject ([ordered]@{ type = 'survivor'; phase = $null })
            }
        }
        try {
            $sharedCheckoutAfter = Get-CheckoutFingerprint -RepositoryRoot $RepositoryRoot
            $evidence | Add-Member -NotePropertyName sharedCheckoutAfter -NotePropertyValue $sharedCheckoutAfter -Force
            if (-not (Test-CheckoutFingerprintEqual `
                -Left $evidence.sharedCheckoutBefore -Right $sharedCheckoutAfter)) {
                $evidence.status = 'failed'
                $evidence.failure = New-OrderedObject ([ordered]@{
                    type = 'artifact_changed'
                    phase = 'shared_checkout_audit'
                })
            }
        } catch {
            $evidence.status = 'failed'
            $evidence.failure = New-OrderedObject ([ordered]@{ type = 'process_error'; phase = 'shared_checkout_audit' })
        }
        $environmentRestoreFailed = $false
        foreach ($name in $savedEnvironment.Keys) {
            try {
                [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name], 'Process')
            } catch {
                $environmentRestoreFailed = $true
            }
        }
        if ($environmentRestoreFailed) {
            $lastStage = 'environment_restore'
            $evidence.status = 'failed'
            $evidence.failure = New-OrderedObject ([ordered]@{ type = 'process_error'; phase = $lastStage })
        }
    }
    $validation = $null
    try {
        $validation = Get-EvidenceSchemaValidation -Evidence $evidence -Stage $lastStage
        if (-not $validation.valid) {
            $evidence = New-SchemaFailureEvidence -Evidence $evidence -Code $validation.code -Stage $validation.stage
        }
        Assert-EvidenceSchema $evidence | Out-Null
    } catch {
        $failureCode = if ($null -ne $validation -and $validation.PSObject.Properties['code']) {
            [string]$validation.code
        } else {
            'EVIDENCE_SCHEMA_CORE'
        }
        $evidence = New-EmergencyEvidenceEnvelope -Evidence $evidence -Code $failureCode -Stage $lastStage
        Assert-EvidenceSchema $evidence | Out-Null
    }
    return $evidence
}

function Invoke-SelfTest {
    $root = Get-RepositoryRoot
    $temp = Join-Path ([IO.Path]::GetTempPath()) "native-rust-incremental-selftest-$PID-$([Guid]::NewGuid().ToString('N'))"
    New-Item -ItemType Directory -Path $temp -Force | Out-Null
    try {
        $workspace = Join-Path $temp 'workspace'
        New-Item -ItemType Directory -Path (Join-Path $workspace 'sakura_core') -Force | Out-Null
        $vcpkgRootPinVerified = $false
        $immutableSnapshotShareVerified = $false
        $packageToolMutationRejectedCount = 0
        $processIdentityPayloadRejected = $false
        $cleanupInspectionVerified = $false
        $argumentQuotingVerified = $false
        $externalSentinelPreserved = $false
        $resolvedApplicationPathVerified = $false
        $rustOutputProviderContractVerified = $false
        $resolvedGitPath = Resolve-ApplicationPath 'git.exe'
        if (-not [IO.Path]::IsPathRooted($resolvedGitPath) -or -not [IO.File]::Exists($resolvedGitPath)) {
            throw 'self-test application resolution did not produce one existing absolute path'
        }
        $resolvedApplicationPathVerified = $true
        $savedAmbientVcpkgRoot = [Environment]::GetEnvironmentVariable('VCPKG_ROOT', 'Process')
        try {
            $env:VCPKG_ROOT = Join-Path $temp 'ambient-poison-vcpkg'
            $pinnedVcpkgRoot = Get-IsolatedVcpkgRoot -Workspace $workspace
            $expectedVcpkgRoot = Get-FullPath (Join-Path $workspace 'tools/vcpkg')
            if (-not $pinnedVcpkgRoot.Equals($expectedVcpkgRoot, [StringComparison]::OrdinalIgnoreCase) -or
                $pinnedVcpkgRoot.Equals((Get-FullPath $env:VCPKG_ROOT), [StringComparison]::OrdinalIgnoreCase)) {
                throw 'self-test ambient vcpkg root was not overridden by the isolated root contract'
            }
            $vcpkgRootPinVerified = $true
        } finally {
            [Environment]::SetEnvironmentVariable('VCPKG_ROOT', $savedAmbientVcpkgRoot, 'Process')
        }
        $log = Join-Path $temp 'synthetic.log'
        $lines = @(
            "Project `"$workspace\sakura_core\sakura.vcxproj`" on node 1",
            '  "cargo" --version',
            '  cargo build --manifest-path rust/native/Cargo.toml',
            '  C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\bin\CL.exe /c "sakura_core\workbench\output\OutputServiceRustProvider.cpp"',
            '  C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\bin\link.exe /OUT:sakura.exe',
            '  C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\bin\lib.exe /OUT:provider.lib',
            '  Deleting file "stale.obj"',
            '  C:\Program Files (x86)\Windows Kits\10\bin\rc.exe /fo"sakura.rc.res" sakura.rc',
            '  C:\Program Files (x86)\Windows Kits\10\bin\mt.exe -manifest sakura.exe.intermediate.manifest',
            '  "C:\Program Files\CMake\bin\cmake.exe" -A x64 -B build -S .',
            '  "C:\Program Files\CMake\bin\cmake.exe" --build build --config Debug',
            '  "C:\work\sakura-senp-tool.exe" componentize guest.wasm extension.wasm',
            '  "C:\work\vcpkg.exe" z-applocal --target-binary=sakura.exe',
            '  "C:\work\sakura-senp-tool.exe" to "C:\work\out\sakura-senp-tool.exe". (TaskId: 9)',
            '  "C:\work\sakura-senp-host.exe" to "C:\work\out\sakura-senp-host.exe". (TaskId: 9)',
            '  C:\Program Files\Build Tools\signcode.exe /sign sakura.exe',
            '  C:\Program Files\vcpkg\vcpkg.exe install --x-manifest-root=.',
            'error MSB4019 C1083 LNK1104 E0425 MSB4019'
        )
        [IO.File]::WriteAllLines($log, $lines, (New-Object Text.UTF8Encoding($false)))
        $diagnosticSummary = Get-DiagnosticLogSummary -LogPath $log
        if (-not $diagnosticSummary.available -or $diagnosticSummary.byteCount -le 0 -or
            $diagnosticSummary.lineCount -ne $lines.Count -or [string]::IsNullOrWhiteSpace([string]$diagnosticSummary.sha256)) {
            throw 'self-test diagnostic log metadata failed'
        }
        if ($diagnosticSummary.errorCodes.MSB4019 -ne 2 -or $diagnosticSummary.errorCodes.C1083 -ne 1 -or
            $diagnosticSummary.errorCodes.LNK1104 -ne 1 -or $diagnosticSummary.errorCodes.E0425 -ne 1 -or
            $diagnosticSummary.errorCodesTruncated) {
            throw 'self-test diagnostic error-code summary failed'
        }
        $classification = Get-ActionClassifications -LogPath $log -Workspace $workspace -Phase 'selftest'
        $actions = @($classification.actions)
        if ($classification.actionRecordCount -ne $actions.Count -or $classification.actionsTruncated -or
            $classification.retainedActionCount -ne $actions.Count -or $classification.workActionCount -ne 11) {
            throw 'self-test action aggregate/bound failed'
        }
        if (@($actions | Where-Object { $_.kind -eq 'cargo-preflight' }).Count -ne 1) { throw 'self-test cargo preflight classification failed' }
        if (@($actions | Where-Object { $_.kind -eq 'cargo' -and $_.operation -eq 'build' }).Count -ne 1) { throw 'self-test cargo build classification failed' }
        foreach ($kind in @('cl', 'link', 'lib', 'rc', 'mt', 'delete')) {
            if (@($actions | Where-Object { $_.kind -eq $kind }).Count -ne 1) { throw "self-test action classification failed: $kind" }
        }
        if (@($actions | Where-Object { $_.kind -eq 'cmake' -and $_.operation -eq 'configure' }).Count -ne 1 -or
            @($actions | Where-Object { $_.kind -eq 'cmake' -and $_.operation -eq 'build' }).Count -ne 1 -or
            @($actions | Where-Object { $_.kind -eq 'senp-tool' -and $_.operation -eq 'componentize' }).Count -ne 1 -or
            @($actions | Where-Object { $_.kind -eq 'vcpkg-applocal' -and $_.operation -eq 'copy-runtime-dependencies' }).Count -ne 1) {
            throw 'self-test build-companion action classification failed'
        }
        if (@($actions | Where-Object { $_.kind -eq 'unexpected_tool' }).Count -ne 2 -or
            $classification.unexpectedToolNames.'signcode.exe' -ne 1 -or
            $classification.unexpectedToolNames.'vcpkg.exe' -ne 1 -or
            $classification.unexpectedToolNamesTruncated) {
            throw 'self-test bounded unknown-tool classification failed'
        }
        if (@(Get-WorkActions $actions).Count -ne 11) { throw 'self-test work-action filtering failed' }
        $preflightViolation = Get-NoOpViolation -Actions @($actions | Where-Object { $_.kind -eq 'cargo-preflight' }) -Phase 'no_op_selftest'
        if ($null -eq $preflightViolation -or $preflightViolation.type -ne 'unexpected_action') { throw 'self-test no-op preflight rejection failed' }
        $resourceManifestViolation = Get-NoOpViolation -Actions @($actions | Where-Object { $_.kind -in @('rc', 'mt') }) -Phase 'no_op_resource_selftest'
        if ($null -eq $resourceManifestViolation -or $resourceManifestViolation.type -ne 'unexpected_action') {
            throw 'self-test no-op resource/manifest rejection failed'
        }
        $companionViolation = Get-NoOpViolation -Actions @($actions | Where-Object {
            $_.kind -in @('cmake', 'senp-tool', 'vcpkg-applocal')
        }) -Phase 'no_op_companion_selftest'
        if ($null -eq $companionViolation -or $companionViolation.type -ne 'unexpected_action') {
            throw 'self-test no-op build-companion rejection failed'
        }
        $toolNameSchemaPhase = New-OrderedObject ([ordered]@{
            unexpectedToolNames = $classification.unexpectedToolNames
            unexpectedToolNamesTruncated = [bool]$classification.unexpectedToolNamesTruncated
            actionCounts = Get-ActionCounts $actions
        })
        Assert-UnexpectedToolNamesSchema -Phase $toolNameSchemaPhase | Out-Null
        $badToolNameSchemaPhase = New-OrderedObject ([ordered]@{
            unexpectedToolNames = (New-OrderedObject ([ordered]@{ '../vcpkg.exe' = 1 }))
            unexpectedToolNamesTruncated = $false
            actionCounts = (New-OrderedObject ([ordered]@{ unexpected_tool = 1 }))
        })
        $unexpectedToolNameSchemaRejected = $false
        try {
            Assert-UnexpectedToolNamesSchema -Phase $badToolNameSchemaPhase | Out-Null
        } catch {
            $unexpectedToolNameSchemaRejected = $true
        }
        if (-not $unexpectedToolNameSchemaRejected) { throw 'self-test unsafe tool-name schema accepted' }
        $emptyLog = Join-Path $temp 'empty.log'
        [IO.File]::WriteAllText($emptyLog, '', (New-Object Text.UTF8Encoding($false)))
        $emptyClassification = Get-ActionClassifications -LogPath $emptyLog -Workspace $workspace -Phase 'empty'
        if ($emptyClassification.actionRecordCount -ne 0 -or $emptyClassification.actions.Count -ne 0 -or
            $emptyClassification.actionsTruncated) { throw 'self-test empty log classification failed' }
        if ($null -ne (Get-NoOpViolation -Actions @() -Phase 'empty')) { throw 'self-test empty no-op acceptance failed' }
        $blankLineLog = Join-Path $temp 'blank-line.log'
        $blankLineLines = @(
            '  cargo build --manifest-path rust/native/Cargo.toml',
            '',
            'Using "CL" task from assembly "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Microsoft\VC\v170\Microsoft.Build.CppTasks.Common.dll".',
            'Using "LIB" task from assembly "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Microsoft\VC\v170\Microsoft.Build.CppTasks.Common.dll".',
            'Using "LINK" task from assembly "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Microsoft\VC\v170\Microsoft.Build.CppTasks.Common.dll".',
            'Using "RC" task from assembly "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Microsoft\VC\v170\rc.exe".',
            'Task Parameter:ToolExe=cl.exe',
            ('  C:\workspace\x64\Debug\sakura.exe (TaskId: 990)' + "`t  "),
            '  C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\bin\cl.exe /c source.cpp'
        )
        [IO.File]::WriteAllLines($blankLineLog, $blankLineLines, (New-Object Text.UTF8Encoding($false)))
        $blankLineClassification = Get-ActionClassifications -LogPath $blankLineLog -Workspace $workspace -Phase 'blank_line'
        if ($blankLineClassification.actionRecordCount -ne 2 -or
            $blankLineClassification.retainedActionCount -ne 2 -or
            $blankLineClassification.actionsTruncated -or
            $blankLineClassification.actionCounts.cargo -ne 1 -or
            $blankLineClassification.actionCounts.cl -ne 1) {
            throw 'self-test blank-line action classification failed'
        }
        if (@(Get-SourcePathMention -Line $blankLineLines[2] -Workspace $workspace).Count -ne 0) {
            throw 'self-test source extension boundary failed'
        }
        $artifactBoundaryLog = Join-Path $temp 'artifact-boundary.log'
        $artifactBoundaryLines = @(
            ('  C:\workspace\x64\Debug\sakura.exe (TaskId: 991)' + "`t "),
            '  C:\workspace\x64\Debug\sakura.pdb (TaskId: 991)',
            '  C:\workspace\x64\Debug\sakura.exe does not exist; source compilation is required. (TaskId: 991)',
            '  C:\workspace\x64\Debug\sakura.exe --verify-build-output (TaskId: 992)',
            '  C:\workspace\x64\Debug\sakura.exe stale output from another task (TaskId: 993)'
        )
        [IO.File]::WriteAllLines($artifactBoundaryLog, $artifactBoundaryLines, (New-Object Text.UTF8Encoding($false)))
        $artifactBoundaryClassification = Get-ActionClassifications -LogPath $artifactBoundaryLog `
            -Workspace $workspace -Phase 'artifact_boundary'
        if ($artifactBoundaryClassification.actionRecordCount -ne 2 -or
            $artifactBoundaryClassification.actionCounts.unexpected_tool -ne 2 -or
            $artifactBoundaryClassification.unexpectedToolNames.'sakura.exe' -ne 2) {
            throw 'self-test executable artifact metadata boundary failed'
        }
        $largeLog = Join-Path $temp 'large.log'
        $largeLines = [System.Collections.Generic.List[string]]::new()
        for ($index = 0; $index -le $script:MaxRetainedActions; $index++) {
            [void]$largeLines.Add('  C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\bin\cl.exe /c source.cpp')
        }
        [IO.File]::WriteAllLines($largeLog, $largeLines.ToArray(), (New-Object Text.UTF8Encoding($false)))
        $largeClassification = Get-ActionClassifications -LogPath $largeLog -Workspace $workspace -Phase 'truncated'
        if ($largeClassification.actionRecordCount -ne ($script:MaxRetainedActions + 1) -or
            $largeClassification.retainedActionCount -ne $script:MaxRetainedActions -or
            $largeClassification.unretainedActionCount -ne 1 -or -not $largeClassification.actionsTruncated -or
            $largeClassification.actionCounts.cl -ne ($script:MaxRetainedActions + 1) -or
            $largeClassification.workActionCount -ne ($script:MaxRetainedActions + 1)) {
            throw 'self-test action truncation/aggregate count failed'
        }
        $truncatedClosure = Get-ExplicitConsumerClosure -Actions @($largeClassification.actions) `
            -ExpectedConsumers @('sakura_core/sakura.vcxproj') -Phase 'truncated' `
            -ActionsTruncated:$largeClassification.actionsTruncated
        if ($truncatedClosure.type -ne 'unexpected_consumer') { throw 'self-test truncated closure fail-closed failed' }
        $truncatedNoOp = Get-NoOpViolation -Actions @($largeClassification.actions) -Phase 'truncated' `
            -ActionsTruncated:$largeClassification.actionsTruncated `
            -AggregateWorkActionCount $largeClassification.workActionCount -AggregateUnexpectedActionCount 0
        if ($truncatedNoOp.type -ne 'unexpected_action' -or $truncatedNoOp.workActionCount -ne ($script:MaxRetainedActions + 1)) {
            throw 'self-test truncated no-op fail-closed failed'
        }
        $nonzeroStdOut = Join-Path $temp 'nonzero.stdout.log'
        $nonzeroStdErr = Join-Path $temp 'nonzero.stderr.log'
        $nonzeroCommand = Get-Command cmd.exe -ErrorAction Stop
        $nonzeroResult = Invoke-BoundedProcess -FilePath $nonzeroCommand.Source `
            -ArgumentList @('/d', '/c', 'ping.exe -n 2 127.0.0.1 >nul & exit 7') `
            -WorkingDirectory $temp -StdOutPath $nonzeroStdOut -StdErrPath $nonzeroStdErr `
            -TimeoutSeconds 10 -Phase 'nonzero_selftest'
        if ($nonzeroResult.type -ne 'build_failed' -or $nonzeroResult.exitCode -ne 7) {
            throw 'self-test nonzero process exit classification failed'
        }
        $timeoutStdOut = Join-Path $temp 'timeout.stdout.log'
        $timeoutStdErr = Join-Path $temp 'timeout.stderr.log'
        $sentinel = $null
        try {
            $sentinelCommand = Get-Command ping.exe -ErrorAction Stop
            $sentinelStart = New-Object Diagnostics.ProcessStartInfo
            $sentinelStart.FileName = $sentinelCommand.Source
            $sentinelStart.Arguments = '-n 10 127.0.0.1'
            $sentinelStart.UseShellExecute = $false
            $sentinelStart.CreateNoWindow = $true
            $sentinel = New-Object Diagnostics.Process
            $sentinel.StartInfo = $sentinelStart
            if (-not $sentinel.Start()) { throw 'self-test external sentinel did not start' }
            $timeoutResult = Invoke-BoundedProcess -FilePath $nonzeroCommand.Source `
                -ArgumentList @('/d', '/c', 'ping.exe -n 30 127.0.0.1 >nul') `
                -WorkingDirectory $temp -StdOutPath $timeoutStdOut -StdErrPath $timeoutStdErr `
                -TimeoutSeconds 1 -Phase 'job_timeout_selftest'
            if ($sentinel.HasExited) { throw 'self-test Job cleanup terminated an external sentinel' }
            $externalSentinelPreserved = $true
        } finally {
            if ($null -ne $sentinel) {
                try {
                    if (-not $sentinel.HasExited) {
                        $sentinel.Kill()
                        if (-not $sentinel.WaitForExit(5000)) {
                            throw 'self-test external sentinel cleanup did not complete'
                        }
                    }
                } finally {
                    $sentinel.Dispose()
                }
            }
        }
        if ($timeoutResult.type -ne 'timeout' -or $timeoutResult.postCleanupSurvivorCount -ne 0) {
            throw 'self-test suspended Job Object timeout cleanup failed'
        }
        $fastStdOut = Join-Path $temp 'fast.stdout.log'
        $fastStdErr = Join-Path $temp 'fast.stderr.log'
        $fastResult = Invoke-BoundedProcess -FilePath $nonzeroCommand.Source `
            -ArgumentList @('/d', '/c', 'echo fast-probe & ping.exe -n 2 127.0.0.1 >nul') `
            -WorkingDirectory $temp -StdOutPath $fastStdOut -StdErrPath $fastStdErr `
            -TimeoutSeconds 10 -Phase 'fast_selftest'
        $fastResult = Add-ProcessOutputMetadata -Result $fastResult -StdOutPath $fastStdOut -StdErrPath $fastStdErr
        if ($fastResult.type -ne 'ok' -or $fastResult.exitCode -ne 0 -or
            $fastResult.outputMetadata.parserFailed -or $fastResult.outputMetadata.stdout.lineCount -ne 1 -or
            [string]$fastResult.outputMetadata.stdout.sha256 -notmatch '^[a-f0-9]{64}$') {
            throw ("self-test fast process finalization failed: type={0}; exit={1}; parserFailed={2}; lines={3}; hash={4}" -f `
                $fastResult.type, $fastResult.exitCode, $fastResult.outputMetadata.parserFailed,
                $fastResult.outputMetadata.stdout.lineCount,
                [bool]([string]$fastResult.outputMetadata.stdout.sha256 -match '^[a-f0-9]{64}$'))
        }
        $argumentEchoScript = Join-Path $temp 'argv-echo.cmd'
        [IO.File]::WriteAllText($argumentEchoScript, "@echo off`r`necho(%~1`r`nping.exe -n 2 127.0.0.1 >nul`r`n", (New-Object Text.ASCIIEncoding))
        $argumentStdOut = Join-Path $temp 'argv.stdout.log'
        $argumentStdErr = Join-Path $temp 'argv.stderr.log'
        $argumentValue = 'argv value with spaces'
        $argumentResult = Invoke-BoundedProcess -FilePath $nonzeroCommand.Source `
            -ArgumentList @('/d', '/c', $argumentEchoScript, $argumentValue) `
            -WorkingDirectory $temp -StdOutPath $argumentStdOut -StdErrPath $argumentStdErr `
            -TimeoutSeconds 10 -Phase 'argument_quoting_selftest'
        if ($argumentResult.type -ne 'ok' -or $argumentResult.exitCode -ne 0 -or
            ([IO.File]::ReadAllText($argumentStdOut)).Trim() -ne $argumentValue) {
            throw 'self-test Windows argument quoting failed'
        }
        $argumentQuotingVerified = $true
        $packageFailureStdOut = Join-Path $temp 'package-failure.stdout.log'
        $packageFailureStdErr = Join-Path $temp 'package-failure.stderr.log'
        [IO.File]::WriteAllText($packageFailureStdOut, "package restore started`r`n", (New-Object Text.UTF8Encoding($false)))
        [IO.File]::WriteAllText($packageFailureStdErr, "BuildError: TOOL_VCVARS_FAILED: unavailable`r`nerror C1083`r`n", (New-Object Text.UTF8Encoding($false)))
        $packageFailureResult = New-OrderedObject ([ordered]@{
            type = 'build_failed'
            phase = 'package_restore'
            exitCode = 3
            durationSeconds = 1.197
            outputAvailable = $true
        })
        $packageFailureResult = Add-ProcessOutputMetadata -Result $packageFailureResult `
            -StdOutPath $packageFailureStdOut -StdErrPath $packageFailureStdErr
        if ($packageFailureResult.type -ne 'build_failed' -or $packageFailureResult.exitCode -ne 3 -or
            $packageFailureResult.outputMetadata.stdout.byteCount -le 0 -or
            $packageFailureResult.outputMetadata.stdout.lineCount -ne 1 -or
            $packageFailureResult.outputMetadata.stderr.lineCount -ne 2 -or
            $packageFailureResult.outputMetadata.buildErrors.TOOL_VCVARS_FAILED -ne 1 -or
            $packageFailureResult.outputMetadata.errorCodes.C1083 -ne 1 -or
            $packageFailureResult.outputMetadata.parserFailed) {
            throw 'self-test package failure output metadata failed'
        }
        $largeFailureLog = Join-Path $temp 'large-package-failure.stderr.log'
        $largeFailureLines = [System.Collections.Generic.List[string]]::new()
        for ($index = 0; $index -lt ($script:MaxProcessFailureCodes + 8); $index++) {
            [void]$largeFailureLines.Add(('BuildError: TOOL_SYNTHETIC_{0:D3}' -f $index))
        }
        for ($index = 0; $index -lt ($script:MaxProcessFailureRecords + 8); $index++) {
            [void]$largeFailureLines.Add('BuildError: TOOL_VCVARS_FAILED')
        }
        [IO.File]::WriteAllLines($largeFailureLog, $largeFailureLines.ToArray(), (New-Object Text.UTF8Encoding($false)))
        $largeFailureMetadata = Get-ProcessOutputMetadata -StdOutPath (Join-Path $temp 'missing-package.stdout.log') `
            -StdErrPath $largeFailureLog
        if (@($largeFailureMetadata.buildErrors.PSObject.Properties).Count -gt $script:MaxProcessFailureCodes -or
            $largeFailureMetadata.failureRecordCount -gt ($script:MaxProcessFailureRecords * 2) -or
            -not $largeFailureMetadata.failureRecordsTruncated) {
            throw 'self-test package failure metadata bounds failed'
        }
        $badOutputPath = Join-Path $temp 'bad-package-output'
        New-Item -ItemType Directory -Path $badOutputPath -Force | Out-Null
        $badOutputMetadata = Get-ProcessOutputMetadata -StdOutPath $badOutputPath -StdErrPath $packageFailureStdErr
        if (-not $badOutputMetadata.parserFailed -or -not $badOutputMetadata.stdout.parserFailed) {
            throw 'self-test package output parser failure status failed'
        }
        $versionProbeStdOut = Join-Path $temp 'vcpkg-version.stdout.log'
        $versionProbeStdErr = Join-Path $temp 'vcpkg-version.stderr.log'
        [IO.File]::WriteAllText($versionProbeStdOut, "vcpkg package management program version 2026-05-27-d5b6777d666e`r`n`r`nSee LICENSE.txt for license information.`r`n", (New-Object Text.UTF8Encoding($false)))
        [IO.File]::WriteAllText($versionProbeStdErr, '', (New-Object Text.UTF8Encoding($false)))
        $versionMetadata = Get-ProcessOutputMetadata -StdOutPath $versionProbeStdOut -StdErrPath $versionProbeStdErr -ExpectedVersionTag '2026-05-27'
        if (-not $versionMetadata.versionMatched -or -not $versionMetadata.versionProofAvailable -or
            $versionMetadata.parserFailed -or $versionMetadata.stdout.lineCount -ne 3) {
            throw 'self-test vcpkg version proof failed'
        }
        $immutableSnapshotPath = Join-Path $temp 'immutable-snapshot.log'
        [IO.File]::WriteAllText($immutableSnapshotPath, 'immutable snapshot', (New-Object Text.UTF8Encoding($false)))
        $immutableReader = [IO.File]::Open($immutableSnapshotPath, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
        $writerOpened = $false
        $deleteAllowed = $false
        try {
            try {
                $writer = [IO.File]::Open($immutableSnapshotPath, [IO.FileMode]::Open, [IO.FileAccess]::Write, [IO.FileShare]::ReadWrite)
                $writerOpened = $true
                $writer.Dispose()
            } catch { }
            try {
                [IO.File]::Delete($immutableSnapshotPath)
                $deleteAllowed = $true
            } catch { }
        } finally {
            $immutableReader.Dispose()
        }
        if ($writerOpened -or $deleteAllowed) {
            throw 'self-test immutable process output sharing failed'
        }
        $immutableSnapshotShareVerified = $true
        $syntheticVcpkgSourcePath = Join-Path $temp 'source-vcpkg.exe'
        $syntheticVcpkgIsolatedPath = Join-Path $temp 'isolated-vcpkg.exe'
        $syntheticVcpkgMarkerPath = Join-Path $temp 'vcpkg.disable-metrics'
        [IO.File]::WriteAllBytes($syntheticVcpkgSourcePath, [byte[]](1, 3, 3, 7))
        [IO.File]::Copy($syntheticVcpkgSourcePath, $syntheticVcpkgIsolatedPath, $false)
        [IO.File]::WriteAllText($syntheticVcpkgMarkerPath, '', (New-Object Text.UTF8Encoding($false)))
        $syntheticVersionProbe = New-OrderedObject ([ordered]@{
            type = 'ok'; phase = 'vcpkg_tool_version'; exitCode = 0; durationSeconds = 0.1
            outputMetadata = $versionMetadata
        })
        $syntheticPackageTool = New-OrderedObject ([ordered]@{
            type = 'ok'
            phase = 'package_tool'
            exitCode = 0
            validation = 'matched'
            reasonCode = $null
            expectedReleaseTag = '2026-05-27'
            source = Get-PayloadFreeFileMetadata -Path $syntheticVcpkgSourcePath
            isolated = Get-PayloadFreeFileMetadata -Path $syntheticVcpkgIsolatedPath
            versionValidation = New-OrderedObject ([ordered]@{
                expectedReleaseTag = '2026-05-27'
                reportedVersionMatched = $true
                proofAvailable = $true
                parserFailed = $false
                recordsTruncated = $false
                probeType = 'ok'
                probeExitCode = 0
            })
            versionProbe = $syntheticVersionProbe
            disableMetricsMarker = Get-PayloadFreeFileMetadata -Path $syntheticVcpkgMarkerPath
        })
        Assert-PackageToolSchema -PackageTool $syntheticPackageTool | Out-Null
        $packageToolMutations = @(
            @{ name = 'expected-tag'; apply = { param($value) $value.expectedReleaseTag = '2026-05-28' } },
            @{ name = 'reported-version-match'; apply = { param($value) $value.versionValidation.reportedVersionMatched = $false } },
            @{ name = 'nested-probe-type'; apply = { param($value) $value.versionProbe.type = 'build_failed' } },
            @{ name = 'nested-probe-exit'; apply = { param($value) $value.versionProbe.exitCode = 7 } },
            @{ name = 'stdout-proof'; apply = { param($value) $value.versionProbe.outputMetadata.stdout.available = $false } },
            @{ name = 'failure-count-equation'; apply = { param($value) $value.versionProbe.outputMetadata.failureRecordCount = 1 } }
        )
        foreach ($mutation in $packageToolMutations) {
            $candidate = $syntheticPackageTool | ConvertTo-Json -Depth 30 | ConvertFrom-Json
            & $mutation['apply'] $candidate
            $rejected = $false
            try { Assert-PackageToolSchema -PackageTool $candidate | Out-Null } catch { $rejected = $true }
            if (-not $rejected) { throw "self-test package tool mutation was accepted: $($mutation['name'])" }
            $packageToolMutationRejectedCount++
        }
        $packageEvidence = New-OrderedObject ([ordered]@{
            schemaVersion = 1
            payloadFree = $true
            phaseOrder = @(
                'baseline', 'no_op_1', 'no_op_2', 'no_op_3', 'rust_source',
                'rust_output_provider', 'cpp_provider', 'package_failure_selftest'
            )
            phases = @()
            packageRestore = $packageFailureResult
            packageTool = $syntheticPackageTool
        })
        Assert-EvidenceSchema $packageEvidence | Out-Null
        $leakyEvidence = $packageEvidence | ConvertTo-Json -Depth 30 | ConvertFrom-Json
        $leakyEvidence.phases = @((New-OrderedObject ([ordered]@{
            name = 'identity_payload_selftest'
            result = (New-OrderedObject ([ordered]@{ type = 'survivor'; processId = 101 }))
        })))
        try {
            Assert-EvidenceSchema $leakyEvidence | Out-Null
        } catch {
            $processIdentityPayloadRejected = $true
        }
        if (-not $processIdentityPayloadRejected) { throw 'self-test raw process identity schema rejection failed' }
        $childRecord = New-OrderedObject ([ordered]@{ name = 'child.exe' })
        $survivorResult = New-ObservedSurvivorResult -Phase 'survivor_selftest' -ExitCode 0 -DurationSeconds 0.1 `
            -Observed @($childRecord) -Remaining @()
        if ($survivorResult.type -ne 'survivor' -or $survivorResult.observedSurvivorCount -ne 1 -or
            $survivorResult.postCleanupSurvivorCount -ne 0) {
            throw 'self-test observed survivor failure semantics failed'
        }
        $compilerHelperRecord = New-OrderedObject ([ordered]@{ name = 'mspdbsrv.exe' })
        $allowedDisposition = Get-OwnedJobMemberDisposition -Records @($compilerHelperRecord) `
            -AllowedHelperNames @('mspdbsrv.exe')
        $deniedDisposition = Get-OwnedJobMemberDisposition -Records @($compilerHelperRecord)
        if (@($allowedDisposition.expected).Count -ne 1 -or @($allowedDisposition.unexpected).Count -ne 0 -or
            @($deniedDisposition.expected).Count -ne 0 -or @($deniedDisposition.unexpected).Count -ne 1) {
            throw 'self-test expected compiler helper policy failed'
        }
        $combinedFailureResult = New-OrderedObject ([ordered]@{
            type = 'survivor'
            phase = 'combined_failure_selftest'
            exitCode = 1
            durationSeconds = 0.2
            observedSurvivorCount = 1
            observedSurvivorNames = (Get-ProcessNameCounts @($childRecord))
            survivors = @()
            postCleanupSurvivorCount = 0
            postCleanupSurvivorNames = (Get-ProcessNameCounts @())
        })
        $combinedFailurePhase = New-OrderedObject ([ordered]@{
            name = 'combined_failure_selftest'
            result = $combinedFailureResult
            diagnostics = $diagnosticSummary
            diagnosticsParseFailed = $false
            actionCounts = Get-ActionCounts $actions
            actionKinds = @($actions | ForEach-Object { [string]$_.kind } | Sort-Object -Unique)
            actions = $actions
            workActionCount = [int](Get-WorkActions $actions).Count
            logAvailable = $true
        })
        $combinedEvidence = New-OrderedObject ([ordered]@{
            schemaVersion = 1
            payloadFree = $true
            phaseOrder = @(
                'baseline', 'no_op_1', 'no_op_2', 'no_op_3', 'rust_source',
                'rust_output_provider', 'cpp_provider', 'combined_failure_selftest'
            )
            phases = @($combinedFailurePhase)
        })
        Assert-EvidenceSchema $combinedEvidence | Out-Null
        if ($combinedFailurePhase.result.type -ne 'survivor' -or $combinedFailurePhase.result.exitCode -ne 1 -or
            $combinedFailurePhase.diagnostics.errorCodes.MSB4019 -ne 2 -or
            $combinedFailurePhase.actionCounts.cargo -ne 1) {
            throw 'self-test combined survivor/build-failure evidence failed'
        }
        $closure = Get-ExplicitConsumerClosure -Actions $actions -ExpectedConsumers @('sakura_core/sakura.vcxproj') -Phase 'selftest'
        if ($closure.type -ne 'ok' -or $closure.actual.Count -ne 1) { throw 'self-test explicit closure failed' }
        $unexpected = Get-ExplicitConsumerClosure -Actions $actions -ExpectedConsumers @('other.vcxproj') -Phase 'selftest'
        if ($unexpected.type -ne 'unexpected_consumer') { throw 'self-test unexpected consumer type failed' }
        $outputProviderActions = @(
            (New-OrderedObject ([ordered]@{
                phase = 'rust_output_provider'; kind = 'cargo'; operation = 'build'
                project = 'sakura_core/sakura.vcxproj'; sourcePaths = @()
            })),
            (New-OrderedObject ([ordered]@{
                phase = 'rust_output_provider'; kind = 'rustc'; operation = 'invoke'
                project = 'sakura_core/sakura.vcxproj'; sourcePaths = @()
            })),
            (New-OrderedObject ([ordered]@{
                phase = 'rust_output_provider'; kind = 'link'; operation = 'invoke'
                project = 'sakura_core/sakura.vcxproj'; sourcePaths = @()
            })),
            (New-OrderedObject ([ordered]@{
                phase = 'rust_output_provider'; kind = 'vcpkg-applocal'
                operation = 'copy-runtime-dependencies'; project = 'sakura_core/sakura.vcxproj'
                sourcePaths = @()
            }))
        )
        $outputProviderClosure = Get-ExplicitConsumerClosure -Actions $outputProviderActions `
            -ExpectedConsumers $script:ExpectedLinkConsumers.rust_output_provider `
            -Phase 'rust_output_provider'
        $outputProviderForbidden = @($outputProviderActions | Where-Object {
            $_.kind -in @('cl', 'lib', 'rc', 'cmake', 'senp-tool', 'delete',
                'cargo-preflight', 'unexpected_tool')
        })
        if ($outputProviderClosure.type -ne 'ok' -or
            @($outputProviderActions | Where-Object { $_.kind -eq 'link' }).Count -ne 1 -or
            @($outputProviderActions | Where-Object {
                $_.kind -eq 'cargo' -and $_.operation -eq 'build'
            }).Count -ne 1 -or $outputProviderForbidden.Count -ne 0) {
            throw 'self-test Rust OutputService provider phase contract failed'
        }
        $rustOutputProviderContractVerified = $true
        $artifactPath = Join-Path $workspace 'artifact.bin'
        [IO.File]::WriteAllBytes($artifactPath, [byte[]](1, 2, 3))
        $snapshot = Get-ArtifactSnapshot -Workspace $workspace -Artifacts @(@{ label = 'artifact'; relativePath = 'artifact.bin' })
        if (-not $snapshot[0].exists -or $snapshot[0].sizeBytes -ne 3 -or [string]::IsNullOrWhiteSpace([string]$snapshot[0].sha256)) { throw 'self-test artifact schema failed' }
        $missing = Get-ArtifactSnapshot -Workspace $workspace -Artifacts @(@{ label = 'missing'; relativePath = 'missing.bin' })
        if ($missing[0].exists -or $null -ne $missing[0].sha256) { throw 'self-test missing artifact schema failed' }
        $cleanupProbe = Join-Path $temp 'cleanup-probe'
        New-Item -ItemType Directory -Path (Join-Path $cleanupProbe 'nested') -Force | Out-Null
        [IO.File]::WriteAllText((Join-Path $cleanupProbe 'nested\file.txt'), 'cleanup', (New-Object Text.UTF8Encoding($false)))
        Assert-NoDirectoryReparsePointsBelow -Path $cleanupProbe -Context 'self-test cleanup tree'
        Remove-DirectoryRecoverable -Path $cleanupProbe
        if ([IO.Directory]::Exists($cleanupProbe) -or [IO.File]::Exists($cleanupProbe)) {
            throw 'self-test cleanup did not remove the complete path'
        }
        $cleanupInspectionVerified = $true
        $synthetic = New-OrderedObject ([ordered]@{
            schemaVersion = 1
            payloadFree = $true
            phaseOrder = @(
                'baseline', 'no_op_1', 'no_op_2', 'no_op_3', 'rust_source',
                'rust_output_provider', 'cpp_provider'
            )
            phases = @((New-OrderedObject ([ordered]@{ name = 'baseline'; result = (New-OrderedObject ([ordered]@{ type = 'ok' })) })))
        })
        Assert-EvidenceSchema $synthetic | Out-Null
        $sharedFingerprintEvidence = New-OrderedObject ([ordered]@{
            schemaVersion = 1
            payloadFree = $true
            status = 'failed'
            phaseOrder = @(
                'baseline', 'no_op_1', 'no_op_2', 'no_op_3', 'rust_source',
                'rust_output_provider', 'cpp_provider'
            )
            phases = @()
            sharedCheckoutBefore = (New-OrderedObject ([ordered]@{ head = 'a'; statusSha256 = ('0' * 64) }))
            sharedCheckoutAfter = (New-OrderedObject ([ordered]@{ head = 'a'; statusSha256 = ('1' * 64) }))
            failure = (New-OrderedObject ([ordered]@{ type = 'artifact_changed'; phase = 'shared_checkout_audit' }))
        })
        Assert-EvidenceSchema $sharedFingerprintEvidence | Out-Null
        $sharedFingerprintEvidence.failure = New-OrderedObject ([ordered]@{ type = 'build_failed'; phase = 'baseline' })
        $untypedSharedFingerprintRejected = $false
        try {
            Assert-EvidenceSchema $sharedFingerprintEvidence | Out-Null
        } catch {
            $untypedSharedFingerprintRejected = $true
        }
        if (-not $untypedSharedFingerprintRejected) {
            throw 'self-test untyped shared fingerprint mismatch was accepted'
        }
        $sharedValidation = Get-EvidenceSchemaValidation -Evidence $sharedFingerprintEvidence -Stage 'baseline'
        if ($sharedValidation.valid -or $sharedValidation.code -ne 'EVIDENCE_SCHEMA_SHARED_CHECKOUT') {
            throw 'self-test shared checkout schema classification failed'
        }
        $sharedFingerprintEvidence.phases = @($synthetic.phases)
        $schemaFailureEnvelope = New-SchemaFailureEvidence -Evidence $sharedFingerprintEvidence `
            -Code $sharedValidation.code -Stage $sharedValidation.stage
        Assert-EvidenceSchema $schemaFailureEnvelope | Out-Null
        if ($schemaFailureEnvelope.status -ne 'failed' -or
            $schemaFailureEnvelope.failure.type -ne 'process_error' -or
            $schemaFailureEnvelope.failure.phase -ne 'baseline' -or
            $schemaFailureEnvelope.schemaValidation.code -ne 'EVIDENCE_SCHEMA_SHARED_CHECKOUT' -or
            @($schemaFailureEnvelope.phases).Count -ne 1) {
            throw 'self-test schema failure envelope did not retain bounded evidence'
        }
        $emergencyEnvelope = New-EmergencyEvidenceEnvelope -Evidence $combinedEvidence `
            -Code $sharedValidation.code -Stage 'baseline'
        Assert-EvidenceSchema $emergencyEnvelope | Out-Null
        if ($emergencyEnvelope.schemaValidation.retention -ne 'bounded_phase_metadata' -or
            @($emergencyEnvelope.phases).Count -ne 1 -or
            $emergencyEnvelope.phases[0].result.type -ne 'survivor' -or
            $emergencyEnvelope.phases[0].result.exitCode -ne 1 -or
            $emergencyEnvelope.phases[0].result.observedSurvivorCount -ne 1 -or
            $emergencyEnvelope.phases[0].result.postCleanupSurvivorCount -ne 0 -or
            $emergencyEnvelope.phases[0].diagnostics.errorCodes.MSB4019 -ne 2 -or
            $emergencyEnvelope.phases[0].actionCounts.cargo -ne 1) {
            throw 'self-test emergency evidence envelope did not retain bounded phase metadata'
        }
        $malformedEmergency = New-EmergencyEvidenceEnvelope -Evidence (
            New-OrderedObject ([ordered]@{ phases = @((New-OrderedObject ([ordered]@{}))) })
        ) -Code 'EVIDENCE_SCHEMA_CORE' -Stage 'baseline'
        Assert-EvidenceSchema $malformedEmergency | Out-Null
        if ($malformedEmergency.phases[0].name -ne 'schema_validation' -or
            $malformedEmergency.phases[0].result.type -ne 'process_error') {
            throw 'self-test malformed emergency evidence was not fail-closed'
        }
        $rawIdentityEvidence = New-OrderedObject ([ordered]@{
            schemaVersion = 1
            payloadFree = $true
            phaseOrder = @(
                'baseline', 'no_op_1', 'no_op_2', 'no_op_3', 'rust_source',
                'rust_output_provider', 'cpp_provider'
            )
            phases = @($synthetic.phases)
            processId = 123
            commandLine = 'must-not-survive'
        })
        $rawValidation = Get-EvidenceSchemaValidation -Evidence $rawIdentityEvidence -Stage 'baseline'
        if ($rawValidation.valid -or $rawValidation.code -ne 'EVIDENCE_SCHEMA_RAW_IDENTITY') {
            throw 'self-test raw identity schema classification failed'
        }
        $rawEnvelope = New-SchemaFailureEvidence -Evidence $rawIdentityEvidence `
            -Code $rawValidation.code -Stage $rawValidation.stage
        Assert-EvidenceSchema $rawEnvelope | Out-Null
        $rawEnvelopeJson = $rawEnvelope | ConvertTo-Json -Depth 30 -Compress
        if ($rawEnvelopeJson -match '(?i)processId|commandLine|must-not-survive') {
            throw 'self-test schema failure envelope retained raw identity'
        }
        $registrationWorkspace = Join-Path $root 'build/tmp/native-rust-incremental/selftest-registration'
        $registrationLines = @(
            "worktree $root",
            'HEAD 0000000000000000000000000000000000000000',
            '',
            "worktree $registrationWorkspace",
            'HEAD 1111111111111111111111111111111111111111'
        )
        if (-not (Get-WorktreeRegistrationState -RepositoryRoot $root -Workspace $registrationWorkspace -Lines $registrationLines)) {
            throw 'self-test exact worktree registration was not found'
        }
        if (Get-WorktreeRegistrationState -RepositoryRoot $root -Workspace (Join-Path $root 'build/tmp/missing') -Lines $registrationLines) {
            throw 'self-test absent worktree registration was accepted'
        }
        $invalidRegistrationInputs = [System.Collections.Generic.List[object]]::new()
        $invalidRegistrationInputs.Add([string[]]@()) | Out-Null
        $invalidRegistrationInputs.Add([string[]]@('worktree')) | Out-Null
        $invalidRegistrationInputs.Add([string[]]@('worktree relative')) | Out-Null
        $invalidRegistrationInputs.Add([string[]]@("worktree $registrationWorkspace")) | Out-Null
        $registrationParserRejectedCount = 0
        foreach ($invalidLines in $invalidRegistrationInputs) {
            try {
                Get-WorktreeRegistrationState -RepositoryRoot $root -Workspace $registrationWorkspace `
                    -Lines ([string[]]$invalidLines) | Out-Null
            } catch {
                $registrationParserRejectedCount++
            }
        }
        if ($registrationParserRejectedCount -ne $invalidRegistrationInputs.Count) {
            throw 'self-test malformed worktree registration output was accepted'
        }
        $fingerprintLeft = New-OrderedObject ([ordered]@{
            head = 'a'
            statusSha256 = ('0' * 64)
            statusLineCount = 1
            submoduleCount = 8
            initializedSubmoduleCount = 8
            submoduleSha256 = ('1' * 64)
        })
        $fingerprintRight = New-OrderedObject ([ordered]@{
            head = 'a'
            statusSha256 = ('0' * 64)
            statusLineCount = 1
            submoduleCount = 8
            initializedSubmoduleCount = 8
            submoduleSha256 = ('1' * 64)
        })
        if (-not (Test-CheckoutFingerprintEqual -Left $fingerprintLeft -Right $fingerprintRight)) {
            throw 'self-test identical checkout fingerprints differ'
        }
        $fingerprintRight.submoduleSha256 = ('2' * 64)
        if (Test-CheckoutFingerprintEqual -Left $fingerprintLeft -Right $fingerprintRight) {
            throw 'self-test submodule fingerprint mutation was accepted'
        }
        $summary = [ordered]@{
            schemaVersion = $script:SchemaVersion
            payloadFree = $true
            actionCounts = Get-ActionCounts $actions
            unexpectedToolNames = $classification.unexpectedToolNames
            unexpectedToolNamesTruncated = [bool]$classification.unexpectedToolNamesTruncated
            workActionCount = [int](Get-WorkActions $actions).Count
            closureType = [string]$closure.type
            unexpectedClosureType = [string]$unexpected.type
            observedSurvivorResultType = [string]$survivorResult.type
            observedSurvivorCount = [int]$survivorResult.observedSurvivorCount
            postCleanupSurvivorCount = [int]$survivorResult.postCleanupSurvivorCount
            expectedCompilerHelperPolicyVerified = $true
            diagnosticByteCount = [UInt64]$diagnosticSummary.byteCount
            diagnosticLineCount = [int]$diagnosticSummary.lineCount
            diagnosticErrorCodes = $diagnosticSummary.errorCodes
            combinedFailureResultType = [string]$combinedFailurePhase.result.type
            combinedFailureExitCode = [int]$combinedFailurePhase.result.exitCode
            combinedFailureDiagnosticErrorCodeCount = [int]$combinedFailurePhase.diagnostics.errorCodes.MSB4019
            retainedActionCount = [int]$classification.retainedActionCount
            actionRecordCount = [int]$classification.actionRecordCount
            actionsTruncated = [bool]$largeClassification.actionsTruncated
            truncatedClosureType = [string]$truncatedClosure.type
            truncatedNoOpType = [string]$truncatedNoOp.type
            nonzeroResultType = [string]$nonzeroResult.type
            nonzeroExitCode = [int]$nonzeroResult.exitCode
            jobBoundTimeoutResultType = [string]$timeoutResult.type
            jobBoundTimeoutPostCleanupCount = [int]$timeoutResult.postCleanupSurvivorCount
            suspendedJobOwnershipVerified = $true
            externalSentinelPreserved = [bool]$externalSentinelPreserved
            fastProcessResultType = [string]$fastResult.type
            fastProcessStdoutShaAvailable = [bool]([string]$fastResult.outputMetadata.stdout.sha256 -match '^[a-f0-9]{64}$')
            packageFailureResultType = [string]$packageFailureResult.type
            packageFailureExitCode = [int]$packageFailureResult.exitCode
            packageFailureStdoutBytes = [UInt64]$packageFailureResult.outputMetadata.stdout.byteCount
            packageFailureStdoutLines = [UInt64]$packageFailureResult.outputMetadata.stdout.lineCount
            packageFailureStderrLines = [UInt64]$packageFailureResult.outputMetadata.stderr.lineCount
            packageFailureBuildErrorCount = [int]$packageFailureResult.outputMetadata.buildErrors.TOOL_VCVARS_FAILED
            packageFailureErrorCodeCount = [int]$packageFailureResult.outputMetadata.errorCodes.C1083
            packageFailureParserFailed = [bool]$packageFailureResult.outputMetadata.parserFailed
            packageFailureRecordsTruncated = [bool]$largeFailureMetadata.failureRecordsTruncated
            versionValidationMatched = [bool]$versionMetadata.versionProofAvailable
            versionOutputLineCount = [int]$versionMetadata.stdout.lineCount
            immutableSnapshotShareVerified = [bool]$immutableSnapshotShareVerified
            cleanupInspectionVerified = [bool]$cleanupInspectionVerified
            argumentQuotingVerified = [bool]$argumentQuotingVerified
            resolvedApplicationPathVerified = [bool]$resolvedApplicationPathVerified
            sharedFingerprintFailureTyped = $true
            schemaFailureEnvelopeVerified = $true
            emergencyEnvelopeVerified = $true
            worktreeRegistrationFailClosedVerified = $true
            worktreeRegistrationRejectedCount = [int]$registrationParserRejectedCount
            submoduleFingerprintComparisonVerified = $true
            packageToolMutationCount = [int]$packageToolMutations.Count
            packageToolMutationRejectedCount = [int]$packageToolMutationRejectedCount
            processIdentityPayloadRejected = [bool]$processIdentityPayloadRejected
            parserFailureObserved = [bool]$badOutputMetadata.parserFailed
            blankLineActionClassificationVerified = $true
            directToolInvocationBoundaryVerified = $true
            rustOutputProviderContractVerified = [bool]$rustOutputProviderContractVerified
            trackedArtifactStatusBoundaryVerified = $true
            sourceExtensionBoundaryVerified = $true
            resourceAndManifestActionClassificationVerified = $true
            incrementalCompanionActionClassificationVerified = $true
            unexpectedToolNameSchemaVerified = [bool]$unexpectedToolNameSchemaRejected
            vcpkgRootPinVerified = [bool]$vcpkgRootPinVerified
            packageToolSchemaVerified = $true
        }
        Write-Output ('SELFTEST_JSON ' + (($summary | ConvertTo-Json -Compress -Depth 10)))
        Write-Output "PASS $script:VerifierName self-tests"
    } finally {
        if ([IO.Directory]::Exists($temp)) { [IO.Directory]::Delete($temp, $true) }
    }
}

if ($SelfTest) {
    Invoke-SelfTest
    exit 0
}

$repoRoot = Get-RepositoryRoot
$outputPath = if ([IO.Path]::IsPathRooted($Output)) { Get-FullPath $Output } else { Get-FullPath (Join-Path $repoRoot $Output) }
Assert-PathBelow -Path $outputPath -Root (Join-Path $repoRoot 'build') -Context 'Output' | Out-Null
Assert-NoReparsePoint -Path (Split-Path -Parent $outputPath) -Context 'Output parent'
New-Item -ItemType Directory -Path (Split-Path -Parent $outputPath) -Force | Out-Null

$evidence = $null
$exitCode = 0
try {
    $evidence = Invoke-Verifier -RepositoryRoot $repoRoot -Platform $Platform -Configuration $Configuration `
        -NoOpIterations $NoOpIterations -TimeoutSeconds $TimeoutSeconds -WorkspaceRoot $WorkspaceRoot -KeepWorkspace:$KeepWorkspace
    if ($evidence.status -ne 'passed') {
        $exitCode = switch ([string]$evidence.failure.type) {
            'timeout' { 8 }
            'survivor' { 9 }
            'missing_output' { 10 }
            'unexpected_consumer' { 11 }
            default { 6 }
        }
    }
} catch {
    $evidence = New-EmergencyEvidenceEnvelope -Evidence $evidence -Code 'EVIDENCE_SCHEMA_CORE' -Stage 'top_level'
    $exitCode = 6
}

try {
    Assert-EvidenceSchema $evidence | Out-Null
} catch {
    $evidence = New-EmergencyEvidenceEnvelope -Evidence $evidence -Code 'EVIDENCE_SCHEMA_CORE' -Stage 'top_level'
    Assert-EvidenceSchema $evidence | Out-Null
    $exitCode = 6
}
$json = $evidence | ConvertTo-Json -Depth 30
[IO.File]::WriteAllText($outputPath, $json, (New-Object Text.UTF8Encoding($false)))
if ($evidence.status -eq 'passed') {
    Write-Output "PASS ${script:VerifierName}: $outputPath"
} else {
    # ErrorActionPreference=Stop would turn Write-Error into exit code 1 and
    # hide the verifier's typed failure code.  Write directly to stderr.
    [Console]::Error.WriteLine("$script:VerifierName failed with typed result $($evidence.failure.type)")
}
exit $exitCode
