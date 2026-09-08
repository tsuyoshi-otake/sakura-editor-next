param(
    [ValidateSet('ViewContainers', 'TreeViews', 'ReadonlyEditors', 'ReadonlyDocuments', 'MixedDocuments', 'TextResources')][string]$ProbeSet = 'ViewContainers',
    [string]$Tests1 = (Join-Path (Split-Path $PSScriptRoot -Parent) 'x64/Debug/tests1.exe'),
    [string]$OutputDirectory = (Join-Path $env:USERPROFILE 'tmp/senp-view-rendering'),
    [ValidateRange(1, 4)][int]$Repetitions = 1,
    [ValidateRange(0, 0.1)][double]$AllowedExcessPercent = 0.05
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$clock = [Diagnostics.Stopwatch]::StartNew()
$exe = (Resolve-Path -LiteralPath $Tests1).Path
$output = [IO.Path]::GetFullPath($OutputDirectory)
[IO.Directory]::CreateDirectory($output) | Out-Null
if (@(Get-CimInstance Win32_Process -Filter "Name='tests1.exe'" | Where-Object { $_.ExecutablePath -eq $exe }).Count) { throw 'The selected tests1 is already running.' }
Add-Type -AssemblyName System.Drawing.Common
$drawingReferences = @([Drawing.Bitmap].Assembly.Location, [Drawing.Rectangle].Assembly.Location)
# Newer PowerShell/.NET versions split Graphics interfaces into a private
# framework assembly; this is an existing runtime dependency, not an install.
foreach ($name in [Drawing.Graphics].Assembly.GetReferencedAssemblies()) {
    if ($name.Name.StartsWith('System.Private.Windows.')) { $drawingReferences += [Reflection.Assembly]::Load($name).Location }
}
Add-Type -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Text;
public static class SenpViewProbe {
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct Point { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)] public struct MonitorInfo { public uint Size; public Rect Monitor, Work; public uint Flags; }
    public delegate bool EnumProc(IntPtr window, IntPtr parameter);
    public delegate bool MonitorEnumProc(IntPtr monitor, IntPtr dc, IntPtr rect, IntPtr parameter);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc callback, IntPtr parameter);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr window, int command);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("user32.dll", SetLastError=true)] public static extern bool SetWindowPos(IntPtr window, IntPtr after, int x, int y, int width, int height, uint flags);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr window, StringBuilder text, int size);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr window, out Rect rect);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr window, out Rect rect);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr window, ref Point point);
    [DllImport("user32.dll")] public static extern bool EnumDisplayMonitors(IntPtr dc, IntPtr clip, MonitorEnumProc callback, IntPtr parameter);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern bool GetMonitorInfoW(IntPtr monitor, ref MonitorInfo info);
    [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(Point point);
    [DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr window, uint flags);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern bool RedrawWindow(IntPtr window, IntPtr rect, IntPtr region, uint flags);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr window, IntPtr dc, uint flags);
    [DllImport("user32.dll", SetLastError=true)] public static extern IntPtr SendMessageTimeoutW(IntPtr window, uint message, IntPtr wparam, IntPtr lparam, uint flags, uint timeout, out IntPtr result);
    [DllImport("dwmapi.dll")] public static extern int DwmFlush();
    public static IntPtr Find(uint pid, string caption) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((window, unused) => { uint candidate; GetWindowThreadProcessId(window, out candidate);
            if (candidate != pid) return true;
            StringBuilder title = new StringBuilder(128); GetWindowTextW(window, title, 128);
            if (title.ToString() != caption) return true;
            found = window; return false; }, IntPtr.Zero);
        return found;
    }
    public static IntPtr Send(IntPtr window, uint message, long command, long parameter) {
        IntPtr result;
        if (SendMessageTimeoutW(window, message, (IntPtr)command, (IntPtr)parameter, 2, 2000, out result) == IntPtr.Zero)
            throw new InvalidOperationException("Native request timed out or failed.");
        return result;
    }
    public static string Geometry(IntPtr window) {
        Rect rect; if (!GetWindowRect(window, out rect)) throw new InvalidOperationException("Missing native window.");
        return rect.Left + "," + rect.Top + "," + rect.Right + "," + rect.Bottom + "," + IsWindowVisible(window);
    }
    public static Bitmap Screen(IntPtr window) {
        Rect client; Point origin = new Point(); GetClientRect(window, out client); ClientToScreen(window, ref origin);
        Bitmap image = new Bitmap(client.Right, client.Bottom, PixelFormat.Format32bppArgb);
        try { using (Graphics graphics = Graphics.FromImage(image))
            graphics.CopyFromScreen(origin.X, origin.Y, 0, 0, image.Size, CopyPixelOperation.SourceCopy);
            return image;
        } catch { image.Dispose(); throw; }
    }
    static bool IsUnoccluded(IntPtr window) {
        Rect client; Point origin = new Point();
        if (!GetClientRect(window, out client) || !ClientToScreen(window, ref origin)) return false;
        for (int y = 1; y < 16; ++y) for (int x = 1; x < 16; ++x) {
            Point point = new Point { X = origin.X + client.Right * x / 16, Y = origin.Y + client.Bottom * y / 16 };
            if (GetAncestor(WindowFromPoint(point), 2) != window) return false;
        }
        return true;
    }
    public static Point PlaceInsideWorkArea(IntPtr window) {
        Rect outer;
        if (!GetWindowRect(window, out outer)) throw new InvalidOperationException("Probe geometry unavailable.");
        int width = outer.Right - outer.Left, height = outer.Bottom - outer.Top;
        Rect[] workAreas = new Rect[16]; int workAreaCount = 0;
        if (!EnumDisplayMonitors(IntPtr.Zero, IntPtr.Zero, (monitor, dc, rect, parameter) => {
            MonitorInfo info = new MonitorInfo { Size = (uint)Marshal.SizeOf<MonitorInfo>() };
            if (GetMonitorInfoW(monitor, ref info) && workAreaCount < workAreas.Length) workAreas[workAreaCount++] = info.Work;
            return true;
        }, IntPtr.Zero)) throw new InvalidOperationException("Desktop work areas unavailable.");
        for (int left = 0; left < workAreaCount; ++left) for (int right = left + 1; right < workAreaCount; ++right) {
            if (workAreas[right].Bottom <= workAreas[left].Bottom) continue;
            Rect swap = workAreas[left]; workAreas[left] = workAreas[right]; workAreas[right] = swap;
        }
        for (int areaIndex = 0; areaIndex < workAreaCount; ++areaIndex) {
            Rect work = workAreas[areaIndex];
            if (width > work.Right - work.Left || height > work.Bottom - work.Top) continue;
            int[] xs = { work.Left + 16, work.Right - width - 16, work.Left + (work.Right - work.Left - width) / 2 };
            int[] ys = { work.Bottom - height - 16, work.Top + 16, work.Top + (work.Bottom - work.Top - height) / 2 };
            foreach (int y in ys) foreach (int x in xs) {
                if (x < work.Left || y < work.Top || x + width > work.Right || y + height > work.Bottom) continue;
                if (!SetWindowPos(window, (IntPtr)(-1), x, y, 0, 0, 0x11)) continue;
                DwmFlush();
                if (!IsUnoccluded(window)) continue;
                Point[] parking = {
                    new Point { X = work.Right - 2, Y = work.Bottom - 2 },
                    new Point { X = work.Left + 2, Y = work.Top + 2 }
                };
                foreach (Point point in parking)
                    if (point.X < x || point.X >= x + width || point.Y < y || point.Y >= y + height) return point;
            }
        }
        throw new InvalidOperationException("No unoccluded work-area placement is available for the owned probe window.");
    }
    public static Bitmap[] Capture(IntPtr window) {
        Rect client, outer; Point origin = new Point();
        if (!GetClientRect(window, out client) || !GetWindowRect(window, out outer) || !ClientToScreen(window, ref origin))
            throw new InvalidOperationException("Capture geometry unavailable.");
        int width = client.Right, height = client.Bottom;
        for (int y = 1; y < 16; ++y) for (int x = 1; x < 16; ++x) {
            Point point = new Point { X = origin.X + width * x / 16, Y = origin.Y + height * y / 16 };
            IntPtr covering = GetAncestor(WindowFromPoint(point), 2);
            if (covering != window) {
                uint coveringPid; GetWindowThreadProcessId(covering, out coveringPid);
                StringBuilder coveringTitle = new StringBuilder(128); GetWindowTextW(covering, coveringTitle, 128);
                throw new InvalidOperationException("Occluded trial at " + point.X + "," + point.Y
                    + "; expected HWND=" + window + "; covering HWND=" + covering + ", PID=" + coveringPid + ", title=" + coveringTitle);
            }
        }
        string stage = "screen allocation";
        Bitmap screen = null, printed = null;
        try {
            screen = new Bitmap(width, height, PixelFormat.Format32bppArgb);
            stage = "CopyFromScreen";
            using (Graphics graphics = Graphics.FromImage(screen)) graphics.CopyFromScreen(origin.X, origin.Y, 0, 0, new Size(width, height), CopyPixelOperation.SourceCopy);
            stage = "window allocation";
            using (Bitmap full = new Bitmap(outer.Right - outer.Left, outer.Bottom - outer.Top, PixelFormat.Format32bppArgb)) {
                stage = "PrintWindow";
                using (Graphics graphics = Graphics.FromImage(full)) {
                    IntPtr dc = graphics.GetHdc();
                    try { if (!PrintWindow(window, dc, 2)) throw new InvalidOperationException("PrintWindow failed."); }
                    finally { graphics.ReleaseHdc(dc); }
                }
                stage = "client crop";
                Rectangle crop = new Rectangle(origin.X - outer.Left, origin.Y - outer.Top, width, height);
                if (crop.Left < 0 || crop.Top < 0 || crop.Right > full.Width || crop.Bottom > full.Height)
                    throw new InvalidOperationException("Client rectangle exceeds the window bitmap.");
                printed = full.Clone(crop, PixelFormat.Format32bppArgb);
            }
            return new Bitmap[] { screen, printed };
        } catch (Exception error) {
            if (screen != null) screen.Dispose(); if (printed != null) printed.Dispose();
            throw new InvalidOperationException("Capture failed at " + stage + "; client=" + width + "x" + height
                + "; origin=" + origin.X + "," + origin.Y + "; outer=" + outer.Left + "," + outer.Top + "," + outer.Right + "," + outer.Bottom
                + "; " + error.Message, error);
        }
    }
    public static double Difference(Bitmap a, Bitmap b, string heatPath) {
        if (a.Size != b.Size) throw new InvalidOperationException("Capture dimensions changed.");
        Rectangle rect = new Rectangle(0, 0, a.Width, a.Height);
        using (Bitmap heat = new Bitmap(a.Width, a.Height, PixelFormat.Format32bppArgb)) {
            BitmapData aa = a.LockBits(rect, ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
            BitmapData bb = b.LockBits(rect, ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
            BitmapData hh = heat.LockBits(rect, ImageLockMode.WriteOnly, PixelFormat.Format32bppArgb);
            long changed = 0;
            try { unsafe {
                for (int y = 0; y < a.Height; ++y) {
                    byte* pa = (byte*)aa.Scan0 + aa.Stride*y; byte* pb = (byte*)bb.Scan0 + bb.Stride*y; byte* ph = (byte*)hh.Scan0 + hh.Stride*y;
                    for (int x = 0; x < a.Width; ++x) {
                        int p = x*4;
                        bool diff = Math.Abs(pa[p]-pb[p])>8 || Math.Abs(pa[p+1]-pb[p+1])>8 || Math.Abs(pa[p+2]-pb[p+2])>8;
                        if (diff) ++changed;
                        ph[p] = ph[p+1] = diff ? (byte)0 : (byte)30; ph[p+2] = diff ? (byte)255 : (byte)30; ph[p+3] = 255;
                    }
                }
            }} finally { a.UnlockBits(aa); b.UnlockBits(bb); heat.UnlockBits(hh); }
            if (!String.IsNullOrEmpty(heatPath)) heat.Save(heatPath, ImageFormat.Png);
            return 100.0 * changed / (a.Width * a.Height);
        }
    }
}
'@ -CompilerOptions '/unsafe' -ReferencedAssemblies $drawingReferences

$process = $null; $window = [IntPtr]::Zero
$results = [Collections.Generic.List[object]]::new()
$receipt = [ordered]@{ status = 'failed'; probeSet = $ProbeSet; tests1 = $exe; binarySha256 = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash; trials = $results; cleanupConfirmed = $false }
function Invoke-Probe([int]$Command, [long]$Parameter = 0) {
    $value = [SenpViewProbe]::Send($window, 0x8296, $Command, $Parameter)
    if ($value -eq [IntPtr]::Zero) { throw "Probe operation $Command failed." }
    return $value
}
function Wait-DocumentReady {
    if ($ProbeSet -notin @('ReadonlyDocuments', 'MixedDocuments')) { return }
    $readyClock = [Diagnostics.Stopwatch]::StartNew()
    $readyBackoff = 5
    while ([SenpViewProbe]::Send($window, 0x8296, 0, 0) -eq [IntPtr]::Zero) {
        if ($readyClock.ElapsedMilliseconds -gt 5000) { throw 'Readonly document preparation/reflow did not finish.' }
        Start-Sleep -Milliseconds $readyBackoff
        $readyBackoff = [Math]::Min(40, $readyBackoff * 2)
    }
}
function Save-Trial([string]$Name, [string]$Before, [string]$After) {
    if ($Before -eq $After) { throw "Gesture $Name did not change native geometry or row state." }
    [void][SenpViewProbe]::DwmFlush()
    $pair = [SenpViewProbe]::Capture($window)
    $floor = $null
    try {
        $prefix = Join-Path $output $Name
        $pair[0].Save($prefix + '-screen.png'); $pair[1].Save($prefix + '-print.png')
        $difference = [SenpViewProbe]::Difference($pair[0], $pair[1], $prefix + '-diff.png')
        [void][SenpViewProbe]::RedrawWindow($window, [IntPtr]::Zero, [IntPtr]::Zero, 0x185)
        [void][SenpViewProbe]::DwmFlush()
        $floor = [SenpViewProbe]::Capture($window)
        $noise = [SenpViewProbe]::Difference($floor[0], $floor[1], $null)
        $drift = [SenpViewProbe]::Difference($pair[0], $floor[0], $prefix + '-redraw-diff.png')
        # Native child controls can transiently omit text from PrintWindow.
        # Keep the original screen and gesture fixed. Up to two additional
        # same-geometry captures may establish the independent print floor;
        # every intervening screen must still agree with the original screen.
        $floorObservations = [Collections.Generic.List[object]]::new()
        $floorObservations.Add([ordered]@{ noisePercent = $noise; redrawDriftPercent = $drift })
        for ($attempt = 1; $attempt -le 2 -and $drift -le $AllowedExcessPercent -and $noise -gt $AllowedExcessPercent -and $difference - $noise -gt $AllowedExcessPercent; ++$attempt) {
            $floor[0].Save($prefix + "-floor$attempt-screen.png"); $floor[1].Save($prefix + "-floor$attempt-print.png")
            foreach ($bitmap in $floor) { $bitmap.Dispose() }; $floor = $null
            [void][SenpViewProbe]::DwmFlush()
            $floor = [SenpViewProbe]::Capture($window)
            $noise = [SenpViewProbe]::Difference($floor[0], $floor[1], $null)
            $drift = [Math]::Max($drift, [SenpViewProbe]::Difference($pair[0], $floor[0], $prefix + "-floor$attempt-drift.png"))
            $floorObservations.Add([ordered]@{ noisePercent = $noise; redrawDriftPercent = $drift })
        }
        $printOnlyOmission = ($difference - $noise -gt $AllowedExcessPercent) -and ($drift -le $AllowedExcessPercent) -and ($noise -le $AllowedExcessPercent)
        $passed = ($drift -le $AllowedExcessPercent) -and (($difference - $noise -le $AllowedExcessPercent) -or $printOnlyOmission)
        $results.Add([ordered]@{ name = $Name; before = $Before; after = $After; differencePercent = $difference; noisePercent = $noise; redrawDriftPercent = $drift; floorObservations = $floorObservations; printOnlyOmission = $printOnlyOmission; passed = $passed })
        if (-not $passed -or $printOnlyOmission) { $floor[0].Save($prefix + '-noise-screen.png'); $floor[1].Save($prefix + '-noise-print.png') }
    } finally { foreach ($bitmap in $pair) { $bitmap.Dispose() }; if ($floor) { foreach ($bitmap in $floor) { $bitmap.Dispose() } } }
}
try {
    $start = [Diagnostics.ProcessStartInfo]::new($exe)
    $start.UseShellExecute = $false; $start.CreateNoWindow = $true; $start.WindowStyle = 'Hidden'
    $start.RedirectStandardOutput = $true; $start.RedirectStandardError = $true
    $suite = if ($ProbeSet -eq 'TextResources') { 'SenpTextResourceViewTest' } elseif ($ProbeSet -eq 'MixedDocuments') { 'SenpReadonlyDocumentHostTest' } elseif ($ProbeSet -eq 'ReadonlyDocuments') { 'SenpReadonlyDocument' } elseif ($ProbeSet -eq 'ReadonlyEditors') { 'SenpReadonlyWorkbench' } elseif ($ProbeSet -eq 'TreeViews') { 'SenpTreeView' } else { 'SenpViewContainer' }
    $caption = if ($ProbeSet -eq 'TextResources') { 'SENP native text verification' } elseif ($ProbeSet -eq 'MixedDocuments') { 'Mixed SENP document' } elseif ($ProbeSet -eq 'ReadonlyDocuments') { 'SENP native document verification' } elseif ($ProbeSet -eq 'ReadonlyEditors') { 'SENP native readonly Editor verification' } elseif ($ProbeSet -eq 'TreeViews') { 'SENP native TreeView verification' } else { 'SENP native ViewContainer verification' }
    $start.ArgumentList.Add("--gtest_filter=$suite.DISABLED_VisualCaptureProbe")
    $start.ArgumentList.Add('--gtest_also_run_disabled_tests')
    $start.Environment['SAKURA_SENP_VIEW_PROBE'] = '1'
    $process = [Diagnostics.Process]::Start($start)
    $stdout = $process.StandardOutput.ReadToEndAsync(); $stderr = $process.StandardError.ReadToEndAsync()
    $receipt.processId = $process.Id
    $ready = [Diagnostics.Stopwatch]::StartNew()
    $backoff = 10
    while ($ready.ElapsedMilliseconds -lt 10000 -and -not $process.HasExited) {
        $window = [SenpViewProbe]::Find($process.Id, $caption)
        if ($window -ne [IntPtr]::Zero -and [SenpViewProbe]::Send($window, 0x8296, 0, 0) -ne [IntPtr]::Zero) { break }
        Start-Sleep -Milliseconds $backoff; $backoff = [Math]::Min(200, $backoff * 2)
    }
    if ($window -eq [IntPtr]::Zero -or $process.HasExited) { throw 'Visual probe did not become ready.' }
    [void](Invoke-Probe 0)
    # The helper process starts hidden; display only its identified visual
    # fixture after native preparation and the command receiver are ready.
    [void][SenpViewProbe]::ShowWindow($window, 5)
    # Only this run-owned probe is made topmost. Its bounded cleanup destroys it;
    # no user window is hidden, restyled or closed to obtain an unoccluded trial.
    $cursorParkingPoint = [SenpViewProbe]::PlaceInsideWorkArea($window)
    [void][SenpViewProbe]::SetForegroundWindow($window)
    [void][SenpViewProbe]::DwmFlush()
    [void][SenpViewProbe]::SetCursorPos($cursorParkingPoint.X, $cursorParkingPoint.Y)
    # Observe the show-animation settling without PrintWindow or forced paint.
    # Stable native rectangles alone cannot prove the composited frame is ready.
    $settling = [Diagnostics.Stopwatch]::StartNew(); $stableFrames = 0
    $previous = [SenpViewProbe]::Screen($window)
    try {
        while ($stableFrames -lt 3 -and $settling.ElapsedMilliseconds -lt 5000) {
            [void][SenpViewProbe]::DwmFlush()
            $current = [SenpViewProbe]::Screen($window)
            if ([SenpViewProbe]::Difference($previous, $current, $null) -le 0.001) { ++$stableFrames } else { $stableFrames = 0 }
            $previous.Dispose(); $previous = $current
        }
        if ($stableFrames -lt 3) { throw 'The first visible frame did not settle.' }
    } finally { $previous.Dispose() }
    $body = Invoke-Probe 5; $header = Invoke-Probe 7
    foreach ($themeId in 0..2) { foreach ($dpi in 96, 144, 192) {
        [void](Invoke-Probe 1 (380 -bor (($dpi -bor ($themeId -shl 10)) -shl 16)))
        Wait-DocumentReady
        for ($repeat = 0; $repeat -lt $Repetitions; ++$repeat) {
            $gestures = if ($ProbeSet -eq 'TextResources') { @('log-visibility', 'resize', 'scroll', 'find', 'append') } elseif ($ProbeSet -eq 'MixedDocuments') { @('section-switch', 'resize', 'mixed-visibility', 'find', 'selection') } elseif ($ProbeSet -eq 'ReadonlyDocuments') { @('document-visibility', 'resize', 'scroll', 'refresh', 'find', 'selection') } elseif ($ProbeSet -eq 'ReadonlyEditors') { @('input-switch', 'resize', 'editor-visibility', 'surface-move') } elseif ($ProbeSet -eq 'TreeViews') { @('expand', 'resize', 'scroll', 'refresh') } else { @('collapse', 'resize', 'view-move', 'container-move') }
            foreach ($gesture in $gestures) { foreach ($direction in 1, 0) {
                if ($clock.Elapsed.TotalSeconds -gt $(if ($ProbeSet -eq 'TextResources') { 520 } elseif ($ProbeSet -eq 'ReadonlyDocuments') { 320 } elseif ($ProbeSet -eq 'ReadonlyEditors') { 200 } else { 140 })) { throw 'Rendering run exceeded its overall deadline.' }
                # The preceding text append deliberately reveals the tail. Reset
                # this next scroll gesture's baseline before measuring its change.
                if ($ProbeSet -eq 'TextResources' -and $gesture -eq 'scroll' -and $direction -eq 1) { [void](Invoke-Probe 3 0) }
                $before = [SenpViewProbe]::Geometry($body)
                if ($ProbeSet -in @('TreeViews', 'ReadonlyEditors', 'ReadonlyDocuments', 'MixedDocuments', 'TextResources')) { $before += ':' + (Invoke-Probe 9).ToString() }
                switch ($gesture) {
                    'log-visibility' { [void](Invoke-Probe 2 $direction) }
                    'mixed-visibility' { [void](Invoke-Probe 3 $direction) }
                    'section-switch' { [void](Invoke-Probe 2 $direction) }
                    'selection' { [void](Invoke-Probe 11 $direction) }
                    'find' { [void](Invoke-Probe 10 $direction) }
                    'append' { [void](Invoke-Probe 8 $direction) }
                    'document-visibility' { [void](Invoke-Probe 2 $direction) }
                    'input-switch' { [void](Invoke-Probe 2 $direction) }
                    'editor-visibility' { [void](Invoke-Probe 3 $direction) }
                    'surface-move' { [void](Invoke-Probe 8 $direction) }
                    'expand' { [void](Invoke-Probe 2 $direction) }
                    'scroll' { [void](Invoke-Probe 3 $direction) }
                    'refresh' { [void](Invoke-Probe 8 $direction) }
                    'collapse' { [void][SenpViewProbe]::Send($header, 0xF5, 0, 0) }
                    'resize' { $width = if ($direction) { 95 } else { 380 }; [void](Invoke-Probe 1 ($width -bor (($dpi -bor ($themeId -shl 10)) -shl 16))) }
                    'view-move' { [void](Invoke-Probe 2 $direction) }
                    'container-move' { [void](Invoke-Probe 3 $direction) }
                }
                Wait-DocumentReady
                $after = [SenpViewProbe]::Geometry($body)
                if ($ProbeSet -in @('TreeViews', 'ReadonlyEditors', 'ReadonlyDocuments', 'MixedDocuments', 'TextResources')) { $after += ':' + (Invoke-Probe 9).ToString() }
                Save-Trial "theme$themeId-dpi$dpi-r$repeat-$gesture-$direction" $before $after
            }}
        }
        Write-Host "Verified theme=$themeId dpi=$dpi trials=$($results.Count) elapsed=$([Math]::Round($clock.Elapsed.TotalSeconds, 1))s"
    }}
    [void](Invoke-Probe 4)
    if (-not $process.WaitForExit(5000)) { throw 'Visual probe failed to exit.' }
    if ($process.ExitCode -ne 0) { throw "Visual probe exit code $($process.ExitCode)." }
    if (@($results | Where-Object { -not $_.passed }).Count) { throw 'Pixel differences exceeded the measured noise floor.' }
    $receipt.status = 'passed'
} catch { $receipt.error = $_.Exception.Message; throw }
finally {
    if ($process) {
        if (-not $process.HasExited) { $process.Kill($true); [void]$process.WaitForExit(5000) }
        $receipt.cleanupConfirmed = $process.HasExited
        $receipt.exitCode = if ($process.HasExited) { $process.ExitCode } else { $null }
        if ($process.HasExited) {
            [IO.File]::WriteAllText((Join-Path $output 'probe-stdout.log'), $stdout.GetAwaiter().GetResult())
            [IO.File]::WriteAllText((Join-Path $output 'probe-stderr.log'), $stderr.GetAwaiter().GetResult())
        }
        $process.Dispose()
    }
    $survivors = @(Get-CimInstance Win32_Process -Filter "Name='tests1.exe'" | Where-Object { $_.ExecutablePath -eq $exe })
    $receipt.survivingProcessIds = @($survivors | ForEach-Object { $_.ProcessId })
    if ($survivors.Count) { $receipt.status = 'failed'; $receipt.cleanupConfirmed = $false }
    $receipt.elapsedSeconds = [Math]::Round($clock.Elapsed.TotalSeconds, 3)
    [IO.File]::WriteAllText((Join-Path $output 'evidence.json'), ($receipt | ConvertTo-Json -Depth 8))
    Write-Host "Rendering status=$($receipt.status) cleanup=$($receipt.cleanupConfirmed) elapsed=$($receipt.elapsedSeconds)s"
}
