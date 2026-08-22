param(
    [Parameter(Mandatory = $true)]
    [string]$ExePath,

    [Parameter(Mandatory = $true)]
    [string]$DocumentPath,

    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory,

    [ValidateRange(4, 240)]
    [int]$SampleCount = 24,

    [ValidateRange(20, 400)]
    [int]$DragSpanPixels = 160,

    [int]$WindowX = 60,

    [int]$WindowY = 60,

    [ValidateRange(480, 7680)]
    [int]$WindowWidth = 1280,

    [ValidateRange(240, 4320)]
    [int]$WindowHeight = 800,

    [ValidateRange(1000, 60000)]
    [int]$CommitTimeoutMilliseconds = 5000,

    # When non-zero, replace DocumentPath with one generated paragraph in the
    # output directory. This exercises the single-block continuation path while
    # keeping the exact fixture beside the captured evidence.
    [ValidateRange(0, 2097152)]
    [int]$GeneratedGiantParagraphCharacters = 0
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Drawing
$drawingAssemblies = @(
    (Join-Path $PSHOME 'System.Drawing.Common.dll'),
    (Join-Path $PSHOME 'System.Drawing.Primitives.dll'),
    (Join-Path $PSHOME 'System.Private.Windows.GdiPlus.dll'),
    (Join-Path $PSHOME 'System.Private.Windows.Core.dll')
)
Add-Type -ReferencedAssemblies $drawingAssemblies -TypeDefinition @'
using System;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Text;

public static class MarkdownPreviewUser32Probe
{
    public const uint WM_COMMAND = 0x0111;
    public const uint WM_CLOSE = 0x0010;
    public const uint WM_CANCELMODE = 0x001F;
    public const uint WM_LBUTTONDOWN = 0x0201;
    public const uint WM_LBUTTONUP = 0x0202;
    public const uint WM_MOUSEMOVE = 0x0200;
    public const uint MK_LBUTTON = 0x0001;
    public const int GWL_STYLE = -16;
    public const long WS_VSCROLL = 0x00200000L;
    public const uint GA_ROOT = 2;
    public const uint RDW_INVALIDATE = 0x0001;
    public const uint RDW_ERASE = 0x0004;
    public const uint RDW_ALLCHILDREN = 0x0080;
    public const uint RDW_UPDATENOW = 0x0100;
    public const int SW_RESTORE = 9;
    public const uint SWP_SHOWWINDOW = 0x0040;
    public const uint SWP_NOSIZE = 0x0001;
    public const uint SWP_NOMOVE = 0x0002;
    public const uint SWP_NOACTIVATE = 0x0010;
    public const uint SMTO_ABORTIFHUNG = 0x0002;
    public const uint SIF_RANGE = 0x0001;
    public const uint SIF_PAGE = 0x0002;
    public const uint SIF_POS = 0x0004;
    public const int SB_VERT = 1;

    [StructLayout(LayoutKind.Sequential)]
    public struct RECT
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
        public int Width { get { return Math.Max(0, Right - Left); } }
        public int Height { get { return Math.Max(0, Bottom - Top); } }
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct POINT
    {
        public int X;
        public int Y;
        public POINT(int x, int y) { X = x; Y = y; }
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct SCROLLINFO
    {
        public uint cbSize;
        public uint fMask;
        public int nMin;
        public int nMax;
        public uint nPage;
        public int nPos;
        public int nTrackPos;
    }

    public sealed class WindowSnapshot
    {
        public long Handle { get; set; }
        public string ClassName { get; set; }
        public string Title { get; set; }
        public int ProcessId { get; set; }
        public RECT Bounds { get; set; }
        public bool Visible { get; set; }
    }

    public sealed class PixelDifference
    {
        public long DifferentPixels { get; set; }
        public long TotalPixels { get; set; }
        public double Ratio { get; set; }
        public int MaximumChannelDelta { get; set; }
        public Bitmap Heatmap { get; set; }
    }

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool EnumWindows(EnumWindowsProc callback, IntPtr lParam);
    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool EnumChildWindows(IntPtr parent, EnumWindowsProc callback, IntPtr lParam);
    private delegate bool EnumWindowsProc(IntPtr hwnd, IntPtr lParam);

    [DllImport("user32.dll")]
    private static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("user32.dll")]
    private static extern bool IsWindow(IntPtr hwnd);
    [DllImport("user32.dll")]
    private static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetClassNameW(IntPtr hwnd, StringBuilder className, int maximum);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetWindowTextW(IntPtr hwnd, StringBuilder title, int maximum);
    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint processId);
    [DllImport("user32.dll")]
    private static extern IntPtr GetAncestor(IntPtr hwnd, uint flags);
    [DllImport("user32.dll")]
    private static extern IntPtr WindowFromPoint(POINT point);
    [DllImport("user32.dll")]
    private static extern bool ScreenToClient(IntPtr hwnd, ref POINT point);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr SendMessageTimeoutW(IntPtr hwnd, uint message, IntPtr wParam,
        IntPtr lParam, uint flags, uint timeout, out UIntPtr result);
    [DllImport("user32.dll")]
    private static extern bool ShowWindow(IntPtr hwnd, int command);
    [DllImport("user32.dll")]
    private static extern bool SetForegroundWindow(IntPtr hwnd);
    [DllImport("user32.dll")]
    private static extern bool BringWindowToTop(IntPtr hwnd);
    [DllImport("user32.dll")]
    private static extern bool SetWindowPos(IntPtr hwnd, IntPtr insertAfter, int x, int y, int width, int height, uint flags);
    [DllImport("user32.dll")]
    private static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")]
    private static extern bool RedrawWindow(IntPtr hwnd, IntPtr updateRect, IntPtr updateRegion, uint flags);
    [DllImport("user32.dll")]
    private static extern bool PrintWindow(IntPtr hwnd, IntPtr dc, uint flags);
    [DllImport("user32.dll", EntryPoint = "GetWindowLongPtrW")]
    private static extern IntPtr GetWindowLongPtr64(IntPtr hwnd, int index);
    [DllImport("user32.dll", EntryPoint = "GetWindowLongW")]
    private static extern int GetWindowLong32(IntPtr hwnd, int index);
    [DllImport("user32.dll")]
    private static extern bool GetScrollInfo(IntPtr hwnd, int bar, ref SCROLLINFO info);
    private static string ClassName(IntPtr hwnd)
    {
        var value = new StringBuilder(256);
        GetClassNameW(hwnd, value, value.Capacity);
        return value.ToString();
    }

    private static string Title(IntPtr hwnd)
    {
        var value = new StringBuilder(1024);
        GetWindowTextW(hwnd, value, value.Capacity);
        return value.ToString();
    }

    private static long Style(IntPtr hwnd)
    {
        return IntPtr.Size == 8 ? GetWindowLongPtr64(hwnd, GWL_STYLE).ToInt64() : GetWindowLong32(hwnd, GWL_STYLE);
    }

    private static IntPtr MakeLParam(int x, int y)
    {
        int packed = (x & 0xffff) | ((y & 0xffff) << 16);
        return new IntPtr(packed);
    }

    public static IntPtr FindTopWindow(int[] processIds)
    {
        var selected = IntPtr.Zero;
        var allowed = processIds ?? new int[0];
        EnumWindows((hwnd, ignored) =>
        {
            uint processId;
            GetWindowThreadProcessId(hwnd, out processId);
            if (Array.IndexOf(allowed, (int)processId) >= 0 && IsWindowVisible(hwnd))
            {
                RECT rect;
                if (GetWindowRect(hwnd, out rect) && rect.Width >= 400 && rect.Height >= 300)
                {
                    selected = hwnd;
                    return false;
                }
            }
            return true;
        }, IntPtr.Zero);
        return selected;
    }

    public static IntPtr FindDescendantByClass(IntPtr parent, string className, bool visibleOnly)
    {
        var selected = IntPtr.Zero;
        EnumChildWindows(parent, (hwnd, ignored) =>
        {
            if ((!visibleOnly || IsWindowVisible(hwnd)) && String.Equals(ClassName(hwnd), className, StringComparison.Ordinal))
            {
                selected = hwnd;
                return false;
            }
            return true;
        }, IntPtr.Zero);
        return selected;
    }

    public static WindowSnapshot[] DescendantsByClass(IntPtr parent, string className)
    {
        var result = new System.Collections.ArrayList();
        EnumChildWindows(parent, (hwnd, ignored) =>
        {
            if (String.Equals(ClassName(hwnd), className, StringComparison.Ordinal))
            {
                uint processId;
                GetWindowThreadProcessId(hwnd, out processId);
                RECT bounds;
                GetWindowRect(hwnd, out bounds);
                result.Add(new WindowSnapshot
                {
                    Handle = hwnd.ToInt64(),
                    ClassName = className,
                    Title = Title(hwnd),
                    ProcessId = (int)processId,
                    Bounds = bounds,
                    Visible = IsWindowVisible(hwnd)
                });
            }
            return true;
        }, IntPtr.Zero);
        return (WindowSnapshot[])result.ToArray(typeof(WindowSnapshot));
    }

    public static WindowSnapshot Snapshot(IntPtr hwnd)
    {
        uint processId;
        GetWindowThreadProcessId(hwnd, out processId);
        RECT bounds;
        GetWindowRect(hwnd, out bounds);
        return new WindowSnapshot
        {
            Handle = hwnd.ToInt64(),
            ClassName = ClassName(hwnd),
            Title = Title(hwnd),
            ProcessId = (int)processId,
            Bounds = bounds,
            Visible = IsWindowVisible(hwnd)
        };
    }

    public static void PrepareWindow(IntPtr hwnd, int x, int y, int width, int height)
    {
        ShowWindow(hwnd, SW_RESTORE);
        // The window is run-owned and terminated in finally. Keeping it above
        // unrelated desktop windows makes the screen-vs-PrintWindow comparison
        // independently valid instead of accepting an occluded frame.
        SetWindowPos(hwnd, new IntPtr(-1), x, y, width, height, SWP_SHOWWINDOW);
        BringWindowToTop(hwnd);
        SetForegroundWindow(hwnd);
    }

    public static void RaiseOwnedWindow(IntPtr hwnd)
    {
        SetWindowPos(hwnd, new IntPtr(-1), 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        BringWindowToTop(hwnd);
    }

    public static void SendCommand(IntPtr hwnd, int functionCode)
    {
        UIntPtr result;
        if (SendMessageTimeoutW(hwnd, WM_COMMAND, new IntPtr(functionCode), IntPtr.Zero,
            SMTO_ABORTIFHUNG, 5000, out result) == IntPtr.Zero)
            throw new TimeoutException("WM_COMMAND timed out");
    }

    public static void SendClose(IntPtr hwnd)
    {
        UIntPtr result;
        if (SendMessageTimeoutW(hwnd, WM_CLOSE, IntPtr.Zero, IntPtr.Zero,
            SMTO_ABORTIFHUNG, 5000, out result) == IntPtr.Zero)
            throw new TimeoutException("WM_CLOSE timed out");
    }

    public static void SendCancelMode(IntPtr hwnd)
    {
        UIntPtr result;
        if (SendMessageTimeoutW(hwnd, WM_CANCELMODE, IntPtr.Zero, IntPtr.Zero,
            SMTO_ABORTIFHUNG, 5000, out result) == IntPtr.Zero)
            throw new TimeoutException("WM_CANCELMODE timed out");
    }

    public static double SendMouse(IntPtr hwnd, uint message, int keyState, int x, int y)
    {
        return SendMouse(hwnd, message, keyState, x, y, 5000);
    }

    public static double SendMouse(IntPtr hwnd, uint message, int keyState, int x, int y,
        uint timeoutMilliseconds)
    {
        var stopwatch = Stopwatch.StartNew();
        UIntPtr result;
        if (SendMessageTimeoutW(hwnd, message, new IntPtr(keyState), MakeLParam(x, y),
            SMTO_ABORTIFHUNG, timeoutMilliseconds, out result) == IntPtr.Zero)
            throw new TimeoutException("mouse message timed out");
        stopwatch.Stop();
        return stopwatch.Elapsed.TotalMilliseconds;
    }

    public static bool IsExistingWindow(long handle)
    {
        return IsWindow(new IntPtr(handle));
    }

    public static POINT ToClient(IntPtr hwnd, int screenX, int screenY)
    {
        var point = new POINT(screenX, screenY);
        ScreenToClient(hwnd, ref point);
        return point;
    }

    public static bool HasVerticalStyle(IntPtr hwnd)
    {
        return (Style(hwnd) & WS_VSCROLL) != 0;
    }

    public static int[] VerticalScrollInfo(IntPtr hwnd)
    {
        var info = new SCROLLINFO();
        info.cbSize = (uint)Marshal.SizeOf(typeof(SCROLLINFO));
        info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
        if (!GetScrollInfo(hwnd, SB_VERT, ref info)) return null;
        return new[] { info.nMin, info.nMax, (int)info.nPage, info.nPos };
    }

    public static bool IsUnoccluded(IntPtr root, RECT bounds, int columns, int rows)
    {
        if (bounds.Width <= 0 || bounds.Height <= 0) return false;
        for (int row = 0; row < rows; ++row)
        {
            int y = bounds.Top + ((row * 2 + 1) * bounds.Height) / (rows * 2);
            for (int column = 0; column < columns; ++column)
            {
                int x = bounds.Left + ((column * 2 + 1) * bounds.Width) / (columns * 2);
                var owner = GetAncestor(WindowFromPoint(new POINT(x, y)), GA_ROOT);
                if (owner != root) return false;
            }
        }
        return true;
    }

    public static string FirstOcclusion(IntPtr root, RECT bounds, int columns, int rows)
    {
        for (int row = 0; row < rows; ++row)
        {
            int y = bounds.Top + ((row * 2 + 1) * bounds.Height) / (rows * 2);
            for (int column = 0; column < columns; ++column)
            {
                int x = bounds.Left + ((column * 2 + 1) * bounds.Width) / (columns * 2);
                var hit = WindowFromPoint(new POINT(x, y));
                var owner = GetAncestor(hit, GA_ROOT);
                if (owner != root)
                {
                    uint processId;
                    GetWindowThreadProcessId(owner, out processId);
                    RECT ownerBounds;
                    GetWindowRect(owner, out ownerBounds);
                    return String.Format("point=({0},{1}) hit=0x{2:X} owner=0x{3:X} class={4} title={5} pid={6} bounds=({7},{8})-({9},{10})",
                        x, y, hit.ToInt64(), owner.ToInt64(), ClassName(owner), Title(owner), processId,
                        ownerBounds.Left, ownerBounds.Top, ownerBounds.Right, ownerBounds.Bottom);
                }
            }
        }
        return String.Empty;
    }

    public static void ParkCursorAway(RECT bounds)
    {
        int x = bounds.Left > 32 ? 4 : bounds.Right + 32;
        int y = bounds.Top > 32 ? 4 : bounds.Bottom + 32;
        SetCursorPos(x, y);
    }

    public static void ForceRedraw(IntPtr hwnd)
    {
        RedrawWindow(hwnd, IntPtr.Zero, IntPtr.Zero,
            RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    }

    public static Bitmap CaptureScreen(RECT bounds)
    {
        var bitmap = new Bitmap(bounds.Width, bounds.Height, PixelFormat.Format32bppArgb);
        using (var graphics = Graphics.FromImage(bitmap))
        {
            graphics.CopyFromScreen(bounds.Left, bounds.Top, 0, 0, new Size(bounds.Width, bounds.Height), CopyPixelOperation.SourceCopy);
        }
        return bitmap;
    }

    public static Bitmap CapturePrintWindow(IntPtr hwnd, RECT bounds)
    {
        var bitmap = new Bitmap(bounds.Width, bounds.Height, PixelFormat.Format32bppArgb);
        using (var graphics = Graphics.FromImage(bitmap))
        {
            IntPtr dc = graphics.GetHdc();
            try
            {
                if (!PrintWindow(hwnd, dc, 2)) throw new InvalidOperationException("PrintWindow failed");
            }
            finally
            {
                graphics.ReleaseHdc(dc);
            }
        }
        return bitmap;
    }

    public static PixelDifference Compare(Bitmap first, Bitmap second, int channelThreshold, bool createHeatmap)
    {
        if (first.Width != second.Width || first.Height != second.Height) throw new ArgumentException("Bitmap dimensions differ");
        var rectangle = new Rectangle(0, 0, first.Width, first.Height);
        var firstData = first.LockBits(rectangle, ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
        var secondData = second.LockBits(rectangle, ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
        Bitmap heatmap = createHeatmap ? new Bitmap(first.Width, first.Height, PixelFormat.Format32bppArgb) : null;
        BitmapData heatmapData = null;
        try
        {
            int bytes = Math.Abs(firstData.Stride) * first.Height;
            var firstPixels = new byte[bytes];
            var secondPixels = new byte[bytes];
            var heatPixels = createHeatmap ? new byte[bytes] : null;
            Marshal.Copy(firstData.Scan0, firstPixels, 0, bytes);
            Marshal.Copy(secondData.Scan0, secondPixels, 0, bytes);
            long different = 0;
            int maximum = 0;
            for (int y = 0; y < first.Height; ++y)
            {
                int row = y * Math.Abs(firstData.Stride);
                for (int x = 0; x < first.Width; ++x)
                {
                    int offset = row + x * 4;
                    int blue = Math.Abs(firstPixels[offset] - secondPixels[offset]);
                    int green = Math.Abs(firstPixels[offset + 1] - secondPixels[offset + 1]);
                    int red = Math.Abs(firstPixels[offset + 2] - secondPixels[offset + 2]);
                    int delta = Math.Max(red, Math.Max(green, blue));
                    maximum = Math.Max(maximum, delta);
                    if (delta > channelThreshold) ++different;
                    if (createHeatmap)
                    {
                        heatPixels[offset] = 0;
                        heatPixels[offset + 1] = (byte)Math.Min(255, delta * 2);
                        heatPixels[offset + 2] = delta > channelThreshold ? (byte)255 : (byte)0;
                        heatPixels[offset + 3] = 255;
                    }
                }
            }
            if (createHeatmap)
            {
                heatmapData = heatmap.LockBits(rectangle, ImageLockMode.WriteOnly, PixelFormat.Format32bppArgb);
                Marshal.Copy(heatPixels, 0, heatmapData.Scan0, bytes);
            }
            long total = (long)first.Width * first.Height;
            return new PixelDifference
            {
                DifferentPixels = different,
                TotalPixels = total,
                Ratio = total == 0 ? 0.0 : (double)different / total,
                MaximumChannelDelta = maximum,
                Heatmap = heatmap
            };
        }
        finally
        {
            if (heatmapData != null) heatmap.UnlockBits(heatmapData);
            first.UnlockBits(firstData);
            second.UnlockBits(secondData);
        }
    }

    public static double BrightRightGutterRatio(Bitmap screen, RECT windowBounds, RECT previewBounds)
    {
        int left = Math.Max(0, previewBounds.Left - windowBounds.Left);
        int top = Math.Max(0, previewBounds.Top - windowBounds.Top);
        int right = Math.Min(screen.Width, previewBounds.Right - windowBounds.Left);
        int bottom = Math.Min(screen.Height, previewBounds.Bottom - windowBounds.Top);
        if (right - left < 36 || bottom - top < 20) return 0.0;
        int brightRows = 0;
        int sampledRows = 0;
        for (int y = top + 4; y < bottom - 4; y += 2)
        {
            double rightLuminance = 0;
            double innerLuminance = 0;
            for (int x = right - 14; x < right - 3; ++x)
            {
                Color pixel = screen.GetPixel(x, y);
                rightLuminance += (pixel.R * 0.2126 + pixel.G * 0.7152 + pixel.B * 0.0722);
            }
            for (int x = right - 30; x < right - 19; ++x)
            {
                Color pixel = screen.GetPixel(x, y);
                innerLuminance += (pixel.R * 0.2126 + pixel.G * 0.7152 + pixel.B * 0.0722);
            }
            rightLuminance /= 11.0;
            innerLuminance /= 11.0;
            if (rightLuminance > 190.0 && rightLuminance - innerLuminance > 55.0) ++brightRows;
            ++sampledRows;
        }
        return sampledRows == 0 ? 0.0 : (double)brightRows / sampledRows;
    }
}
'@

function Get-RepositorySakuraProcesses {
    param([string]$ResolvedExe)
    $comparison = [System.IO.Path]::GetFullPath($ResolvedExe)
    @(Get-CimInstance Win32_Process -Filter "Name = 'sakura.exe'" -ErrorAction SilentlyContinue |
        Where-Object {
            $_.ExecutablePath -and
            [System.IO.Path]::GetFullPath([string]$_.ExecutablePath).Equals(
                $comparison, [System.StringComparison]::OrdinalIgnoreCase)
        })
}

function Remove-OwnedProfile {
    param([string]$ProfilePath, [string]$ProfileRoot, [string]$ProfileName)
    $resolvedRoot = [System.IO.Path]::GetFullPath($ProfileRoot).TrimEnd([System.IO.Path]::DirectorySeparatorChar)
    $resolvedPath = [System.IO.Path]::GetFullPath($ProfilePath)
    $prefix = $resolvedRoot + [System.IO.Path]::DirectorySeparatorChar
    if (-not $resolvedPath.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to delete profile outside the Sakura profile root: $resolvedPath"
    }
    if (-not [System.IO.Path]::GetFileName($resolvedPath).Equals($ProfileName, [System.StringComparison]::Ordinal)) {
        throw "Refusing to delete a profile whose leaf does not match the run-owned name: $resolvedPath"
    }
    if ([System.IO.Directory]::Exists($resolvedPath)) {
        [System.IO.Directory]::Delete($resolvedPath, $true)
    }
}

function Get-Percentile {
    param([double[]]$Values, [double]$Percentile)
    if ($Values.Count -eq 0) { return 0.0 }
    $ordered = @($Values | Sort-Object)
    $index = [Math]::Ceiling(($Percentile / 100.0) * $ordered.Count) - 1
    $index = [Math]::Max(0, [Math]::Min($ordered.Count - 1, $index))
    [double]$ordered[$index]
}

function Get-PreviewAlignedOverlays {
    param([object[]]$Overlays, [object]$PreviewSnapshot)
    @($Overlays | Where-Object {
        $_.Visible -and
        [Math]::Abs($_.Bounds.Right - $PreviewSnapshot.Bounds.Right) -le 2 -and
        [Math]::Abs($_.Bounds.Top - $PreviewSnapshot.Bounds.Top) -le 2 -and
        [Math]::Abs($_.Bounds.Bottom - $PreviewSnapshot.Bounds.Bottom) -le 2
    })
}

function Get-ParentFirstProcesses {
    param([object[]]$Processes)
    $byId = @{}
    foreach ($process in $Processes) { $byId[[int]$process.ProcessId] = $process }
    @($Processes | ForEach-Object {
        $depth = 0
        $cursor = $_
        $seen = [System.Collections.Generic.HashSet[int]]::new()
        while ($null -ne $cursor -and $seen.Add([int]$cursor.ProcessId)) {
            $parentId = [int]$cursor.ParentProcessId
            if (-not $byId.ContainsKey($parentId)) { break }
            ++$depth
            $cursor = $byId[$parentId]
        }
        [pscustomobject]@{ Depth = $depth; Process = $_ }
    } | Sort-Object Depth | ForEach-Object { $_.Process })
}

$resolvedExe = [System.IO.Path]::GetFullPath($ExePath)
$resolvedDocument = [System.IO.Path]::GetFullPath($DocumentPath)
$resolvedOutput = [System.IO.Path]::GetFullPath($OutputDirectory)
if (-not [System.IO.File]::Exists($resolvedExe)) { throw "Executable not found: $resolvedExe" }
if (-not [System.IO.File]::Exists($resolvedDocument)) { throw "Document not found: $resolvedDocument" }
[System.IO.Directory]::CreateDirectory($resolvedOutput) | Out-Null
if ($GeneratedGiantParagraphCharacters -gt 0) {
    $resolvedDocument = [System.IO.Path]::Combine($resolvedOutput, 'generated-giant-paragraph.md')
    $fixture = [string]::new('x', $GeneratedGiantParagraphCharacters)
    [System.IO.File]::WriteAllText($resolvedDocument, $fixture, [System.Text.UTF8Encoding]::new($false))
}

# Process ownership is identified relative to the executable's pre-run PID set,
# so two probes for the same binary would misclassify each other's windows and
# cleanup targets. Fail closed instead of allowing an invalid concurrent trial.
$mutexBytes = [System.Security.Cryptography.SHA256]::HashData(
    [System.Text.Encoding]::UTF8.GetBytes($resolvedExe.ToUpperInvariant()))
$mutexName = 'Local\SakuraMarkdownPreviewResize-' +
    [Convert]::ToHexString($mutexBytes).Substring(0, 24)
$probeMutex = [System.Threading.Mutex]::new($false, $mutexName)
$mutexAcquired = $false
try {
    $mutexAcquired = $probeMutex.WaitOne(0)
}
catch [System.Threading.AbandonedMutexException] {
    $mutexAcquired = $true
}
if (-not $mutexAcquired) {
    $probeMutex.Dispose()
    throw "Another Markdown preview resize probe is already using this executable: $resolvedExe"
}

$profileName = 'codex-md-resize-' + [Guid]::NewGuid().ToString('N')
$profileRoot = [System.IO.Path]::Combine($env:APPDATA, 'sakura')
$profilePath = [System.IO.Path]::Combine($profileRoot, $profileName)
Remove-OwnedProfile -ProfilePath $profilePath -ProfileRoot $profileRoot -ProfileName $profileName

$beforeProcesses = @(Get-RepositorySakuraProcesses -ResolvedExe $resolvedExe)
$beforeIds = [System.Collections.Generic.HashSet[int]]::new()
foreach ($process in $beforeProcesses) { [void]$beforeIds.Add([int]$process.ProcessId) }

$runProcessIds = [System.Collections.Generic.HashSet[int]]::new()
$inputProcess = $null
$topWindow = [IntPtr]::Zero
$previewWindow = [IntPtr]::Zero
$samples = [System.Collections.Generic.List[object]]::new()
$noiseSamples = [System.Collections.Generic.List[object]]::new()
$previewNoiseSamples = [System.Collections.Generic.List[object]]::new()
$terminalState = 'failed'
$failure = $null
$lifecycle = $null
$cancelProbe = $null
$commitMilliseconds = $null

try {
    $arguments = '-PROF="{0}" "{1}"' -f $profileName, $resolvedDocument
    $inputProcess = Start-Process -FilePath $resolvedExe -ArgumentList $arguments -PassThru
    [void]$runProcessIds.Add($inputProcess.Id)

    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    while ([DateTime]::UtcNow -lt $deadline -and $topWindow -eq [IntPtr]::Zero) {
        Start-Sleep -Milliseconds 50
        $current = @(Get-RepositorySakuraProcesses -ResolvedExe $resolvedExe)
        foreach ($process in $current) {
            if (-not $beforeIds.Contains([int]$process.ProcessId)) {
                [void]$runProcessIds.Add([int]$process.ProcessId)
            }
        }
        $topWindow = [MarkdownPreviewUser32Probe]::FindTopWindow(@($runProcessIds))
    }
    if ($topWindow -eq [IntPtr]::Zero) { throw 'No run-owned editor window appeared.' }

    [MarkdownPreviewUser32Probe]::PrepareWindow(
        $topWindow, $WindowX, $WindowY, $WindowWidth, $WindowHeight)
    $documentLeaf = [System.IO.Path]::GetFileName($resolvedDocument)
    $readyDeadline = [DateTime]::UtcNow.AddSeconds(20)
    do {
        $readySnapshot = [MarkdownPreviewUser32Probe]::Snapshot($topWindow)
        if ($readySnapshot.Title.IndexOf($documentLeaf, [System.StringComparison]::OrdinalIgnoreCase) -ge 0) {
            break
        }
        Start-Sleep -Milliseconds 50
    } while ([DateTime]::UtcNow -lt $readyDeadline)
    if ($readySnapshot.Title.IndexOf($documentLeaf, [System.StringComparison]::OrdinalIgnoreCase) -lt 0) {
        throw "Editor window never reported the requested document in its title: $($readySnapshot.Title)"
    }
    Start-Sleep -Milliseconds 500
    [MarkdownPreviewUser32Probe]::SendCommand($topWindow, 30998)

    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    while ([DateTime]::UtcNow -lt $deadline -and $previewWindow -eq [IntPtr]::Zero) {
        Start-Sleep -Milliseconds 25
        $previewWindow = [MarkdownPreviewUser32Probe]::FindDescendantByClass(
            $topWindow, 'SakuraMarkdownPreview', $true)
    }
    if ($previewWindow -eq [IntPtr]::Zero) { throw 'The Markdown preview command did not create a visible preview.' }

    Start-Sleep -Milliseconds 250
    $windowSnapshot = [MarkdownPreviewUser32Probe]::Snapshot($topWindow)
    [MarkdownPreviewUser32Probe]::ParkCursorAway($windowSnapshot.Bounds)
    [MarkdownPreviewUser32Probe]::ForceRedraw($topWindow)

    for ($noiseIndex = 0; $noiseIndex -lt 3; ++$noiseIndex) {
        $windowSnapshot = [MarkdownPreviewUser32Probe]::Snapshot($topWindow)
        [MarkdownPreviewUser32Probe]::RaiseOwnedWindow($topWindow)
        if (-not [MarkdownPreviewUser32Probe]::IsUnoccluded($topWindow, $windowSnapshot.Bounds, 7, 5)) {
            $detail = [MarkdownPreviewUser32Probe]::FirstOcclusion($topWindow, $windowSnapshot.Bounds, 7, 5)
            throw "Window was occluded during noise-floor sample $noiseIndex. $detail"
        }
        $screen = [MarkdownPreviewUser32Probe]::CaptureScreen($windowSnapshot.Bounds)
        $print = [MarkdownPreviewUser32Probe]::CapturePrintWindow($topWindow, $windowSnapshot.Bounds)
        try {
            $difference = [MarkdownPreviewUser32Probe]::Compare($screen, $print, 12, $false)
            $noiseSamples.Add([pscustomobject]@{
                index = $noiseIndex
                differentPixels = $difference.DifferentPixels
                totalPixels = $difference.TotalPixels
                ratio = $difference.Ratio
                maximumChannelDelta = $difference.MaximumChannelDelta
            })
        }
        finally {
            $screen.Dispose()
            $print.Dispose()
        }
        $previewSnapshot = [MarkdownPreviewUser32Probe]::Snapshot($previewWindow)
        $previewScreen = [MarkdownPreviewUser32Probe]::CaptureScreen($previewSnapshot.Bounds)
        $previewPrint = [MarkdownPreviewUser32Probe]::CapturePrintWindow($previewWindow, $previewSnapshot.Bounds)
        try {
            $previewDifference = [MarkdownPreviewUser32Probe]::Compare(
                $previewScreen, $previewPrint, 12, $false)
            $previewNoiseSamples.Add([pscustomobject]@{
                index = $noiseIndex
                differentPixels = $previewDifference.DifferentPixels
                totalPixels = $previewDifference.TotalPixels
                ratio = $previewDifference.Ratio
                maximumChannelDelta = $previewDifference.MaximumChannelDelta
            })
        }
        finally {
            $previewScreen.Dispose()
            $previewPrint.Dispose()
        }
        [MarkdownPreviewUser32Probe]::ForceRedraw($topWindow)
    }

    $noiseFloor = [double](($noiseSamples | Measure-Object -Property ratio -Maximum).Maximum)
    $previewNoiseFloor = [double](($previewNoiseSamples | Measure-Object -Property ratio -Maximum).Maximum)
    $previewSnapshot = [MarkdownPreviewUser32Probe]::Snapshot($previewWindow)
    $startScreenX = $previewSnapshot.Bounds.Left - 1
    $startScreenY = $previewSnapshot.Bounds.Top + [Math]::Max(1, [int]($previewSnapshot.Bounds.Height / 2))
    $startClient = [MarkdownPreviewUser32Probe]::ToClient($topWindow, $startScreenX, $startScreenY)
    [void][MarkdownPreviewUser32Probe]::SendMouse(
        $topWindow, [MarkdownPreviewUser32Probe]::WM_LBUTTONDOWN,
        [MarkdownPreviewUser32Probe]::MK_LBUTTON, $startClient.X, $startClient.Y)

    for ($index = 0; $index -lt $SampleCount; ++$index) {
        $half = [Math]::Max(2, [int]($SampleCount / 2))
        $phase = $index % $half
        $fraction = if ($half -le 1) { 0.0 } else { $phase / [double]($half - 1) }
        if (([int]($index / $half) % 2) -ne 0) { $fraction = 1.0 - $fraction }
        $targetScreenX = [int][Math]::Round($startScreenX - ($DragSpanPixels / 2.0) + ($DragSpanPixels * $fraction))
        $targetClient = [MarkdownPreviewUser32Probe]::ToClient($topWindow, $targetScreenX, $startScreenY)
        $messageMilliseconds = [MarkdownPreviewUser32Probe]::SendMouse(
            $topWindow, [MarkdownPreviewUser32Probe]::WM_MOUSEMOVE,
            [MarkdownPreviewUser32Probe]::MK_LBUTTON, $targetClient.X, $targetClient.Y)
        $windowSnapshot = [MarkdownPreviewUser32Probe]::Snapshot($topWindow)
        $previewSnapshot = [MarkdownPreviewUser32Probe]::Snapshot($previewWindow)
        [MarkdownPreviewUser32Probe]::ParkCursorAway($windowSnapshot.Bounds)
        [MarkdownPreviewUser32Probe]::RaiseOwnedWindow($topWindow)
        if (-not [MarkdownPreviewUser32Probe]::IsUnoccluded($topWindow, $windowSnapshot.Bounds, 7, 5)) {
            $detail = [MarkdownPreviewUser32Probe]::FirstOcclusion($topWindow, $windowSnapshot.Bounds, 7, 5)
            throw "Window was occluded during drag sample $index. $detail"
        }

        $screen = [MarkdownPreviewUser32Probe]::CaptureScreen($windowSnapshot.Bounds)
        $print = [MarkdownPreviewUser32Probe]::CapturePrintWindow($topWindow, $windowSnapshot.Bounds)
        $difference = $null
        $heatmap = $null
        $previewScreen = $null
        $previewPrint = $null
        $previewHeatmap = $null
        try {
            $difference = [MarkdownPreviewUser32Probe]::Compare($screen, $print, 12, $true)
            $heatmap = $difference.Heatmap
            $aboveNoise = $difference.Ratio -gt ($noiseFloor + 0.00001)
            $previewScreen = [MarkdownPreviewUser32Probe]::CaptureScreen($previewSnapshot.Bounds)
            $previewPrint = [MarkdownPreviewUser32Probe]::CapturePrintWindow(
                $previewWindow, $previewSnapshot.Bounds)
            $previewDifference = [MarkdownPreviewUser32Probe]::Compare(
                $previewScreen, $previewPrint, 12, $true)
            $previewHeatmap = $previewDifference.Heatmap
            $previewAboveNoise = $previewDifference.Ratio -gt ($previewNoiseFloor + 0.00001)
            if ($aboveNoise -or $index -eq 0 -or $index -eq ($SampleCount - 1)) {
                $prefix = 'frame-{0:D3}' -f $index
                $screen.Save([System.IO.Path]::Combine($resolvedOutput, $prefix + '-screen.png'), [System.Drawing.Imaging.ImageFormat]::Png)
                $print.Save([System.IO.Path]::Combine($resolvedOutput, $prefix + '-print.png'), [System.Drawing.Imaging.ImageFormat]::Png)
                $heatmap.Save([System.IO.Path]::Combine($resolvedOutput, $prefix + '-diff.png'), [System.Drawing.Imaging.ImageFormat]::Png)
            }
            if ($previewAboveNoise -or $index -eq 0 -or $index -eq ($SampleCount - 1)) {
                $prefix = 'frame-{0:D3}-preview' -f $index
                $previewScreen.Save([System.IO.Path]::Combine($resolvedOutput, $prefix + '-screen.png'), [System.Drawing.Imaging.ImageFormat]::Png)
                $previewPrint.Save([System.IO.Path]::Combine($resolvedOutput, $prefix + '-print.png'), [System.Drawing.Imaging.ImageFormat]::Png)
                $previewHeatmap.Save([System.IO.Path]::Combine($resolvedOutput, $prefix + '-diff.png'), [System.Drawing.Imaging.ImageFormat]::Png)
            }

            $overlays = @([MarkdownPreviewUser32Probe]::DescendantsByClass(
                $topWindow, 'SakuraWorkbenchOverlayScrollbar'))
            $visibleOverlays = @($overlays | Where-Object { $_.Visible })
            $previewOverlays = @(Get-PreviewAlignedOverlays -Overlays $overlays -PreviewSnapshot $previewSnapshot)
            $scrollInfo = [MarkdownPreviewUser32Probe]::VerticalScrollInfo($previewWindow)
            $samples.Add([pscustomobject]@{
                index = $index
                targetScreenX = $targetScreenX
                messageMilliseconds = $messageMilliseconds
                previewLeft = $previewSnapshot.Bounds.Left
                previewRight = $previewSnapshot.Bounds.Right
                previewWidth = $previewSnapshot.Bounds.Width
                previewHeight = $previewSnapshot.Bounds.Height
                hasWsVscroll = [MarkdownPreviewUser32Probe]::HasVerticalStyle($previewWindow)
                overlayCount = $overlays.Count
                visibleOverlayCount = $visibleOverlays.Count
                previewOverlayCount = $previewOverlays.Count
                scrollMinimum = if ($null -eq $scrollInfo) { $null } else { $scrollInfo[0] }
                scrollMaximum = if ($null -eq $scrollInfo) { $null } else { $scrollInfo[1] }
                scrollPage = if ($null -eq $scrollInfo) { $null } else { $scrollInfo[2] }
                scrollPosition = if ($null -eq $scrollInfo) { $null } else { $scrollInfo[3] }
                unoccluded = $true
                differentPixels = $difference.DifferentPixels
                totalPixels = $difference.TotalPixels
                differenceRatio = $difference.Ratio
                maximumChannelDelta = $difference.MaximumChannelDelta
                aboveNoiseFloor = $aboveNoise
                previewDifferentPixels = $previewDifference.DifferentPixels
                previewTotalPixels = $previewDifference.TotalPixels
                previewDifferenceRatio = $previewDifference.Ratio
                previewAboveNoiseFloor = $previewAboveNoise
                brightRightGutterRatio = [MarkdownPreviewUser32Probe]::BrightRightGutterRatio(
                    $screen, $windowSnapshot.Bounds, $previewSnapshot.Bounds)
            })
        }
        finally {
            if ($null -ne $heatmap) { $heatmap.Dispose() }
            if ($null -ne $previewHeatmap) { $previewHeatmap.Dispose() }
            if ($null -ne $previewScreen) { $previewScreen.Dispose() }
            if ($null -ne $previewPrint) { $previewPrint.Dispose() }
            $screen.Dispose()
            $print.Dispose()
        }
    }

    $lastPreview = [MarkdownPreviewUser32Probe]::Snapshot($previewWindow)
    $lastClient = [MarkdownPreviewUser32Probe]::ToClient(
        $topWindow, $lastPreview.Bounds.Left - 1,
        $lastPreview.Bounds.Top + [Math]::Max(1, [int]($lastPreview.Bounds.Height / 2)))
    $commitMilliseconds = [MarkdownPreviewUser32Probe]::SendMouse(
        $topWindow, [MarkdownPreviewUser32Probe]::WM_LBUTTONUP, 0,
        $lastClient.X, $lastClient.Y, [uint32]$CommitTimeoutMilliseconds)
    [MarkdownPreviewUser32Probe]::ForceRedraw($topWindow)

    # A capture-loss/cancel branch is a terminal state, not an unfinished drag.
    # Move to a different width, send WM_CANCELMODE directly, and prove that the
    # last committed width is restored before the lifecycle checks continue.
    $committedPreview = [MarkdownPreviewUser32Probe]::Snapshot($previewWindow)
    $cancelStartX = $committedPreview.Bounds.Left - 1
    $cancelY = $committedPreview.Bounds.Top + [Math]::Max(1, [int]($committedPreview.Bounds.Height / 2))
    $cancelStart = [MarkdownPreviewUser32Probe]::ToClient($topWindow, $cancelStartX, $cancelY)
    [void][MarkdownPreviewUser32Probe]::SendMouse(
        $topWindow, [MarkdownPreviewUser32Probe]::WM_LBUTTONDOWN,
        [MarkdownPreviewUser32Probe]::MK_LBUTTON, $cancelStart.X, $cancelStart.Y)
    $cancelMove = [MarkdownPreviewUser32Probe]::ToClient($topWindow, $cancelStartX - 30, $cancelY)
    [void][MarkdownPreviewUser32Probe]::SendMouse(
        $topWindow, [MarkdownPreviewUser32Probe]::WM_MOUSEMOVE,
        [MarkdownPreviewUser32Probe]::MK_LBUTTON, $cancelMove.X, $cancelMove.Y)
    $transientPreview = [MarkdownPreviewUser32Probe]::Snapshot($previewWindow)
    [MarkdownPreviewUser32Probe]::SendCancelMode($topWindow)
    [MarkdownPreviewUser32Probe]::ForceRedraw($topWindow)
    $cancelledPreview = [MarkdownPreviewUser32Probe]::Snapshot($previewWindow)
    $cancelProbe = [pscustomobject]@{
        committedWidth = $committedPreview.Bounds.Width
        transientWidth = $transientPreview.Bounds.Width
        cancelledWidth = $cancelledPreview.Bounds.Width
        transientGeometryChanged = $transientPreview.Bounds.Width -ne $committedPreview.Bounds.Width
        committedGeometryRestored = $cancelledPreview.Bounds.Width -eq $committedPreview.Bounds.Width
    }
    if (-not $cancelProbe.transientGeometryChanged) {
        throw 'WM_CANCELMODE probe did not first change the preview geometry.'
    }
    if (-not $cancelProbe.committedGeometryRestored) {
        throw 'WM_CANCELMODE did not restore the committed preview width.'
    }
    $lastPreview = $cancelledPreview

    $overlaysBeforeClose = @([MarkdownPreviewUser32Probe]::DescendantsByClass(
        $topWindow, 'SakuraWorkbenchOverlayScrollbar'))
    $previewOverlaysBeforeClose = @(Get-PreviewAlignedOverlays `
        -Overlays $overlaysBeforeClose -PreviewSnapshot $lastPreview)
    [MarkdownPreviewUser32Probe]::SendCommand($topWindow, 30998)
    $closeDeadline = [DateTime]::UtcNow.AddSeconds(5)
    do {
        Start-Sleep -Milliseconds 25
        $visiblePreviewAfterClose = [MarkdownPreviewUser32Probe]::FindDescendantByClass(
            $topWindow, 'SakuraMarkdownPreview', $true)
    } while ($visiblePreviewAfterClose -ne [IntPtr]::Zero -and [DateTime]::UtcNow -lt $closeDeadline)
    $overlaysAfterClose = @([MarkdownPreviewUser32Probe]::DescendantsByClass(
        $topWindow, 'SakuraWorkbenchOverlayScrollbar'))
    $oldOverlaySurvivors = @($previewOverlaysBeforeClose | Where-Object {
        [MarkdownPreviewUser32Probe]::IsExistingWindow($_.Handle)
    })

    [MarkdownPreviewUser32Probe]::SendCommand($topWindow, 30998)
    $reopenDeadline = [DateTime]::UtcNow.AddSeconds(5)
    $reopenedPreview = [IntPtr]::Zero
    do {
        Start-Sleep -Milliseconds 25
        $reopenedPreview = [MarkdownPreviewUser32Probe]::FindDescendantByClass(
            $topWindow, 'SakuraMarkdownPreview', $true)
    } while ($reopenedPreview -eq [IntPtr]::Zero -and [DateTime]::UtcNow -lt $reopenDeadline)
    if ($visiblePreviewAfterClose -ne [IntPtr]::Zero) { throw 'Markdown preview remained visible after close.' }
    if ($reopenedPreview -eq [IntPtr]::Zero) { throw 'Markdown preview did not reopen.' }
    $reopenedSnapshot = [MarkdownPreviewUser32Probe]::Snapshot($reopenedPreview)
    $overlaysAfterReopen = @([MarkdownPreviewUser32Probe]::DescendantsByClass(
        $topWindow, 'SakuraWorkbenchOverlayScrollbar'))
    $previewOverlaysAfterReopen = @(Get-PreviewAlignedOverlays `
        -Overlays $overlaysAfterReopen -PreviewSnapshot $reopenedSnapshot)
    [MarkdownPreviewUser32Probe]::SendClose($reopenedPreview)
    $destroyDeadline = [DateTime]::UtcNow.AddSeconds(5)
    do {
        Start-Sleep -Milliseconds 25
        $reopenedPreviewExists = [MarkdownPreviewUser32Probe]::IsExistingWindow($reopenedPreview.ToInt64())
    } while ($reopenedPreviewExists -and [DateTime]::UtcNow -lt $destroyDeadline)
    $overlaysAfterDestroy = @([MarkdownPreviewUser32Probe]::DescendantsByClass(
        $topWindow, 'SakuraWorkbenchOverlayScrollbar'))
    $destroyedOverlaySurvivors = @($previewOverlaysAfterReopen | Where-Object {
        [MarkdownPreviewUser32Probe]::IsExistingWindow($_.Handle)
    })
    $lifecycle = [pscustomobject]@{
        totalBeforeClose = $overlaysBeforeClose.Count
        previewAlignedBeforeClose = $previewOverlaysBeforeClose.Count
        totalAfterToggleHide = $overlaysAfterClose.Count
        hiddenPreviewOverlayHandlesAlive = $oldOverlaySurvivors.Count
        totalAfterReopen = $overlaysAfterReopen.Count
        previewAlignedAfterReopen = $previewOverlaysAfterReopen.Count
        totalAfterPreviewDestroy = $overlaysAfterDestroy.Count
        destroyedPreviewOverlaySurvivors = $destroyedOverlaySurvivors.Count
    }
    $terminalState = 'completed'
}
catch {
    $failure = $_.Exception.ToString()
}
finally {
    if ($topWindow -ne [IntPtr]::Zero) {
        try {
            $snapshot = [MarkdownPreviewUser32Probe]::Snapshot($topWindow)
            $client = [MarkdownPreviewUser32Probe]::ToClient($topWindow, $snapshot.Bounds.Left + 4, $snapshot.Bounds.Top + 4)
            [void][MarkdownPreviewUser32Probe]::SendMouse(
                $topWindow, [MarkdownPreviewUser32Probe]::WM_LBUTTONUP, 0, $client.X, $client.Y)
        }
        catch {}
    }

    $owned = @(Get-RepositorySakuraProcesses -ResolvedExe $resolvedExe |
        Where-Object { -not $beforeIds.Contains([int]$_.ProcessId) })
    foreach ($process in @(Get-ParentFirstProcesses -Processes $owned)) {
        try { Stop-Process -Id ([int]$process.ProcessId) -ErrorAction Stop }
        catch {}
    }
    $cleanupDeadline = [DateTime]::UtcNow.AddSeconds(10)
    do {
        Start-Sleep -Milliseconds 100
        $survivors = @(Get-RepositorySakuraProcesses -ResolvedExe $resolvedExe |
            Where-Object { -not $beforeIds.Contains([int]$_.ProcessId) })
    } while ($survivors.Count -gt 0 -and [DateTime]::UtcNow -lt $cleanupDeadline)

    try { Remove-OwnedProfile -ProfilePath $profilePath -ProfileRoot $profileRoot -ProfileName $profileName }
    catch {
        if ($null -eq $failure) { $failure = $_.Exception.ToString() }
    }

    $durations = [double[]]@($samples | ForEach-Object { [double]$_.messageMilliseconds })
    $summary = [ordered]@{
        terminalState = $terminalState
        failure = $failure
        executable = $resolvedExe
        document = $resolvedDocument
        generatedGiantParagraphCharacters = $GeneratedGiantParagraphCharacters
        outputDirectory = $resolvedOutput
        profileName = $profileName
        sampleCountRequested = $SampleCount
        sampleCountCompleted = $samples.Count
        noiseFloorRatio = if ($noiseSamples.Count -eq 0) { $null } else {
            [double](($noiseSamples | Measure-Object -Property ratio -Maximum).Maximum)
        }
        previewNoiseFloorRatio = if ($previewNoiseSamples.Count -eq 0) { $null } else {
            [double](($previewNoiseSamples | Measure-Object -Property ratio -Maximum).Maximum)
        }
        messageMilliseconds = [ordered]@{
            median = if ($durations.Count -eq 0) { $null } else { Get-Percentile -Values $durations -Percentile 50 }
            p95 = if ($durations.Count -eq 0) { $null } else { Get-Percentile -Values $durations -Percentile 95 }
            maximum = if ($durations.Count -eq 0) { $null } else { [double](($durations | Measure-Object -Maximum).Maximum) }
        }
        commitMilliseconds = $commitMilliseconds
        leakedNativeScrollbarFrames = @($samples | Where-Object {
            $_.hasWsVscroll -or $_.brightRightGutterRatio -gt 0.20
        }).Count
        framesAboveNoiseFloor = @($samples | Where-Object { $_.aboveNoiseFloor }).Count
        previewFramesAboveNoiseFloor = @($samples | Where-Object { $_.previewAboveNoiseFloor }).Count
        previewGeometryChanged = (@($samples | Select-Object -ExpandProperty previewWidth -Unique).Count -gt 1)
        scrollbarLifecycle = $lifecycle
        cancelProbe = $cancelProbe
        noiseSamples = @($noiseSamples)
        previewNoiseSamples = @($previewNoiseSamples)
        samples = @($samples)
        runOwnedProcessIds = @($runProcessIds)
        survivingRunOwnedProcesses = @($survivors | Select-Object ProcessId, ParentProcessId, Name, ExecutablePath, CommandLine)
        profileDirectoryExistsAfterCleanup = [System.IO.Directory]::Exists($profilePath)
    }
    $jsonPath = [System.IO.Path]::Combine($resolvedOutput, 'summary.json')
    $json = $summary | ConvertTo-Json -Depth 8
    [System.IO.File]::WriteAllText($jsonPath, $json, [System.Text.UTF8Encoding]::new($false))
    $json
    if ($mutexAcquired) {
        $probeMutex.ReleaseMutex()
        $mutexAcquired = $false
    }
    $probeMutex.Dispose()
}

if ($terminalState -ne 'completed' -or $null -ne $failure) { exit 1 }
