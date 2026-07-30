# Requires -Version 5.1
<#
.SYNOPSIS
Measures the time required for Sakura Editor to present a Markdown document.

.DESCRIPTION
Each fresh measurement uses a generated profile below the executable directory.
The profile name, process identity records, and path checks are deliberate: this
script must never close an already-running editor or delete an arbitrary folder.
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
    [switch]$SelfTest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$startupTimeoutMs = 30000
$closeTimeoutMs = 3000
$pollIntervalMs = 25

Add-Type -TypeDefinition @'
using System;
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

public static class NativeStartupProbe
{
    private const uint WM_CLOSE = 0x0010;
    private const uint TH32CS_SNAPPROCESS = 0x00000002;
    private const uint PROCESS_QUERY_LIMITED_INFORMATION = 0x1000;
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
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool QueryFullProcessImageNameW(IntPtr process, uint flags, StringBuilder path, ref int size);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GetProcessTimes(IntPtr process, out FILETIME creation, out FILETIME exit, out FILETIME kernel, out FILETIME user);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool CloseHandle(IntPtr handle);

    public static StartupProbeWindow[] GetTopLevelWindows()
    {
        var windows = new List<StartupProbeWindow>();
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
        return windows.ToArray();
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
        var result = new List<StartupProbeProcessEntry>();
        IntPtr snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return result.ToArray();
        try {
            var entry = new PROCESSENTRY32();
            entry.Size = unchecked((uint)Marshal.SizeOf(typeof(PROCESSENTRY32)));
            if (!Process32FirstW(snapshot, ref entry)) return result.ToArray();
            do {
                result.Add(new StartupProbeProcessEntry {
                    ProcessId = unchecked((int)entry.ProcessId),
                    ParentProcessId = unchecked((int)entry.ParentProcessId),
                    ImageName = entry.ExeFile
                });
            } while (Process32NextW(snapshot, ref entry));
        }
        finally { CloseHandle(snapshot); }
        return result.ToArray();
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
'@ -ReferencedAssemblies 'System.Drawing.dll'

function Get-NormalizedPath([string]$Path) {
    return [IO.Path]::GetFullPath($Path).TrimEnd('\\').ToUpperInvariant()
}

function Test-SamePath([string]$Left, [string]$Right) {
    return (Get-NormalizedPath $Left) -eq (Get-NormalizedPath $Right)
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
        $identity = [NativeStartupProbe]::GetProcessIdentity([int]$entry.ProcessId, [int]$entry.ParentProcessId)
        if ($null -eq $identity) { continue }
        $record = Convert-ProcessIdentity $identity
        $result[$record.Id] = $record
    }
    return $result
}

function Get-ProcessesForImagePath([string]$ImagePath) {
    $imageName = [IO.Path]::GetFileName($ImagePath)
    $result = @()
    foreach ($entry in @([NativeStartupProbe]::GetProcessEntries())) {
        if (-not [string]::Equals([string]$entry.ImageName, $imageName, [StringComparison]::OrdinalIgnoreCase)) { continue }
        $identity = [NativeStartupProbe]::GetProcessIdentity([int]$entry.ProcessId, [int]$entry.ParentProcessId)
        if ($null -eq $identity) { continue }
        $record = Convert-ProcessIdentity $identity
        if (Test-SamePath $record.ImagePath $ImagePath) { $result += $record }
    }
    return $result
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

function Get-LiveOwnedProcesses($Owned) {
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

function Get-Statistics([double[]]$Values) {
    $ordered = @($Values | Sort-Object)
    if ($ordered.Count -eq 0) { return $null }
    $middle = [int][Math]::Floor($ordered.Count / 2)
    if (($ordered.Count % 2) -eq 1) { $median = [double]$ordered[$middle] }
    else { $median = ([double]$ordered[$middle - 1] + [double]$ordered[$middle]) / 2.0 }
    return [ordered]@{
        count = $ordered.Count
        medianMs = $median
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
        Remove-Item -LiteralPath $ProfilePath -Recurse -Force -ErrorAction Stop
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

function Stop-OwnedProcesses($Owned) {
    $closeWatch = [Diagnostics.Stopwatch]::StartNew()
    $visibleWindows = [NativeStartupProbe]::GetTopLevelWindows() | Where-Object { $Owned.ContainsKey([int]$_.ProcessId) -and $_.Visible }
    foreach ($window in @($visibleWindows)) {
        [void][NativeStartupProbe]::RequestClose($window.Handle)
    }

    # A process that owned a visible editor window can remain alive as Sakura's hidden
    # control process. Treat destruction of every run-owned visible window as editor
    # closure, then ask every remaining run-owned top-level window to shut down.
    $editorWindowsClosed = Wait-WithPoll {
        return @([NativeStartupProbe]::GetTopLevelWindows() | Where-Object {
            $Owned.ContainsKey([int]$_.ProcessId) -and $_.Visible
        }).Count -eq 0
    } ([Math]::Min(3000, $closeTimeoutMs))
    if ($editorWindowsClosed) {
        $controlWindows = [NativeStartupProbe]::GetTopLevelWindows() | Where-Object {
            $Owned.ContainsKey([int]$_.ProcessId)
        }
        foreach ($window in @($controlWindows)) { [void][NativeStartupProbe]::RequestClose($window.Handle) }
    }

    $remainingGraceMs = [Math]::Max(1, $closeTimeoutMs - [int]$closeWatch.ElapsedMilliseconds)
    if ($editorWindowsClosed) {
        # Sakura's hidden control process can intentionally outlive every editor and
        # may ignore WM_CLOSE. Once all visible editor windows have disappeared, give the
        # control process a short courtesy interval; profile finalization no longer
        # depends on it, and every later force-stop is still identity-scoped to this run.
        $remainingGraceMs = [Math]::Min(1000, $remainingGraceMs)
    }
    $graceful = Wait-WithPoll { @(Get-LiveOwnedProcesses $Owned).Count -eq 0 } $remainingGraceMs
    if (-not $graceful) {
        # Stop parents first. A live parent is the only process that could create a new child.
        $deadline = [Diagnostics.Stopwatch]::StartNew()
        while ($deadline.ElapsedMilliseconds -lt $closeTimeoutMs) {
            $live = @(Get-LiveOwnedProcesses $Owned)
            if ($live.Count -eq 0) { break }
            foreach ($record in @($live | Sort-Object @{ Expression = { Get-OwnedProcessDepth $_ $Owned } }, Id)) {
                $snapshot = Get-ProcessSnapshot $Owned
                if (-not (Test-ProcessIdentity $record $snapshot)) { continue }
                try { Stop-Process -Id $record.Id -Force -ErrorAction Stop } catch { }
            }
            Start-Sleep -Milliseconds $pollIntervalMs
        }
    }
    return @(Get-LiveOwnedProcesses $Owned)
}

function Invoke-StartupMeasurement([string]$Condition, [int]$Iteration, [string]$ExePath, [string]$DocumentPath, [int]$ExpectedLineCount, [string]$ProfileName, [string]$ProfilePath, [string]$ExeDirectory, [bool]$TakeScreenshot, [string]$ScreenshotPath, [string]$TraceDirectory) {
    $owned = @{}
    $traceLaunchQpc = [int64]0
    $traceLaunchFrequency = [int64]0
    $result = [ordered]@{
        condition = $Condition; iteration = $Iteration; profileName = $ProfileName
        processApiReturnMs = $null; topLevelHwndMs = $null; visibleMs = $null; dwmFlushMs = $null
        captionReadyMs = $null; inputIdleMs = $null; inputIdleReached = $false; inputIdleError = $null
        documentReadyMs = $null; verticalScrollMaximum = $null
        startupTrace = $null
        screenshotPath = $null; success = $false; error = $null
        processCleanupVerified = $false; profileCleanupVerified = $false; cleanupVerified = $false
        survivors = @()
    }
    try {
        $arguments = '-PROF="{0}" "{1}"' -f $ProfileName, $DocumentPath
        $startInfo = New-Object Diagnostics.ProcessStartInfo
        $startInfo.FileName = $ExePath
        $startInfo.Arguments = $arguments
        $startInfo.UseShellExecute = $false
        $watch = [Diagnostics.Stopwatch]::StartNew()
        $traceLaunchQpc = [Diagnostics.Stopwatch]::GetTimestamp()
        $traceLaunchFrequency = [Diagnostics.Stopwatch]::Frequency
        $startInfo.EnvironmentVariables['SAKURA_STARTUP_TRACE_DIR'] = $TraceDirectory
        $process = [Diagnostics.Process]::Start($startInfo)
        $result.processApiReturnMs = [Math]::Round($watch.Elapsed.TotalMilliseconds, 3)
        $startedProcessId = $process.Id
        $process.Dispose()
        $snapshot = Get-ProcessSnapshot @{} ([int[]]@($startedProcessId))
        if (-not $snapshot.ContainsKey($startedProcessId)) { throw "Started process $startedProcessId was not observable." }
        $seed = $snapshot[$startedProcessId]
        if (-not (Test-SamePath $seed.ImagePath $ExePath)) { throw 'Started process image path did not match SakuraExe.' }
        $owned[$seed.Id] = $seed
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
        if ($TakeScreenshot -and $null -ne $selectedWindow) {
            if ([NativeStartupProbe]::SaveWindowPng($selectedWindow.Handle, $ScreenshotPath)) { $result.screenshotPath = $ScreenshotPath }
            else { throw 'Screen capture failed for the selected Sakura window.' }
        }
        $result.success = $true
    }
    catch { $result.error = $_.Exception.Message }
    finally {
        try {
            $survivors = @(Stop-OwnedProcesses $owned)
            $result.survivors = @($survivors | ForEach-Object { [ordered]@{ pid = $_.Id; creation = $_.Creation; imagePath = $_.ImagePath; parentPid = $_.ParentId } })
            $result.processCleanupVerified = $survivors.Count -eq 0
            if (-not $result.processCleanupVerified) { throw 'Owned Sakura processes survived cleanup.' }
        }
        catch {
            $result.processCleanupVerified = $false
            $result.success = $false
            $cleanupError = "Process cleanup failed: $($_.Exception.Message)"
            if ([string]::IsNullOrEmpty([string]$result.error)) { $result.error = $cleanupError }
            else { $result.error = "$($result.error) $cleanupError" }
        }
    }
    if ($result.processCleanupVerified) {
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
    if ($statistics.meanMs -ne 20 -or $statistics.minMs -ne 10 -or $statistics.maxMs -ne 30) { throw 'Statistics self-test failed.' }
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
            Remove-Item -LiteralPath $selfTestTraceDirectory -Recurse -Force -ErrorAction Stop
        }
    }
    return [ordered]@{
        selfTest = $true
        passed = $true
        warmNativeProcessSnapshotMs = [Math]::Round($snapshotWatch.Elapsed.TotalMilliseconds, 3)
        timestampUtc = [DateTime]::UtcNow.ToString('o')
    }
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

    $exeDirectory = Split-Path -Parent $exePath
    $documentInfo = Get-Item -LiteralPath $documentPath
    $exeInfo = Get-Item -LiteralPath $exePath
    $lines = @(Get-Content -LiteralPath $documentPath).Count
    if ($lines -le 30) {
        throw "SampleMarkdown must contain at least 31 physical lines so document readiness cannot match Sakura's initial scrollbar placeholder. Actual: $lines."
    }
    $runs = New-Object Collections.Generic.List[object]
    for ($iteration = 1; $iteration -le $Iterations; $iteration++) {
        $profileName = 'startup-probe-{0}' -f ([Guid]::NewGuid().ToString('N'))
        $profilePath = Join-Path $exeDirectory $profileName
        Assert-OwnedProfilePath $profilePath $exeDirectory $profileName
        if (Test-Path -LiteralPath $profilePath) { throw "Generated profile directory already exists: $profilePath" }
        $iterationRuns = New-Object Collections.Generic.List[object]
        $profileCleanupError = $null
        try {
            $freshImage = Join-Path $outputPath ('startup-performance-{0}-fresh-iteration-{1}.png' -f $runId, $iteration)
            $freshTraceDirectory = New-StartupTraceDirectory $outputPath $runId $iteration 'fresh'
            $freshRun = Invoke-StartupMeasurement 'fresh' $iteration $exePath $documentPath $lines $profileName $profilePath $exeDirectory ($CaptureScreenshot -and $iteration -eq 1) $freshImage $freshTraceDirectory
            $iterationRuns.Add($freshRun)
            $runs.Add($freshRun)
            if ($CompareExistingProfile) {
                $existingImage = Join-Path $outputPath ('startup-performance-{0}-existing-profile-iteration-{1}.png' -f $runId, $iteration)
                $existingTraceDirectory = New-StartupTraceDirectory $outputPath $runId $iteration 'existing-profile'
                $existingRun = Invoke-StartupMeasurement 'existingProfile' $iteration $exePath $documentPath $lines $profileName $profilePath $exeDirectory ($CaptureScreenshot -and $iteration -eq 1) $existingImage $existingTraceDirectory
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
        cleanupVerified = -not (@($runs | Where-Object { -not $_.cleanupVerified }).Count)
        configuration = [ordered]@{ sakuraExe = $exePath; sampleMarkdown = $documentPath; iterations = $Iterations; compareExistingProfile = [bool]$CompareExistingProfile; captureScreenshot = [bool]$CaptureScreenshot; measureDwmFlush = [bool]$MeasureDwmFlush; startupTraceEnvironmentVariable = 'SAKURA_STARTUP_TRACE_DIR'; outputDirectory = $outputPath }
        input = [ordered]@{ bytes = [int64]$documentInfo.Length; lines = $lines; sha256 = (Get-Sha256 $documentPath) }
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
