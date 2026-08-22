param(
	[string]$Executable = (Join-Path (Split-Path $PSScriptRoot -Parent | Split-Path -Parent) 'x64\Debug\sakura.exe'),
	[string]$DocumentPath,
	[string]$WorkspaceFolder,
	[ValidateRange(1, 1000)]
	[int]$Trials = 10,
	[ValidateSet('Resize', 'SideBarResize', 'Command')]
	[string]$Gesture = 'Resize',
	[ValidateSet('Default', 'Explorer', 'Search', 'SourceControl')]
	[string]$ActivityBarPage = 'Default',
	[string]$SearchQuery,
	[int]$FunctionCode = 0,
	[ValidateRange(100, 10000)]
	[int]$ReadyTimeoutMilliseconds = 5000,
	[ValidateRange(0, 255)]
	[int]$ChannelTolerance = 8,
	[ValidateRange(0.0, 100.0)]
	[double]$AllowedExcessPercent = 0.05,
	[switch]$PresentedScreenOnly,
	[switch]$FailOnExcess,
	[string]$ProfileName = "codex-render-coherence-$PID",
	[string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
	$repositoryRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
	$OutputDirectory = Join-Path $repositoryRoot ("build\results\render-coherence\{0:yyyyMMdd-HHmmss}" -f (Get-Date))
}

Add-Type -AssemblyName System.Drawing.Common
$drawingAssembly = [Drawing.Bitmap].Assembly.Location
$drawingPrimitivesAssembly = [Drawing.Rectangle].Assembly.Location

Add-Type -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Text;

public static class SakuraFrameCoherenceNative
{
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }

    [StructLayout(LayoutKind.Sequential)]
    public struct POINT { public int X, Y; }

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);

    [DllImport("dwmapi.dll")]
    public static extern int DwmGetWindowAttribute(IntPtr hwnd, int attribute,
        out RECT value, int valueSize);

    [DllImport("user32.dll")]
    public static extern IntPtr WindowFromPoint(POINT point);

    [DllImport("user32.dll")]
    public static extern IntPtr GetAncestor(IntPtr hwnd, uint flags);

    [DllImport("user32.dll")]
    private static extern IntPtr GetParent(IntPtr hwnd);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool SetWindowPos(IntPtr hwnd, IntPtr insertAfter,
        int x, int y, int width, int height, uint flags);

    [DllImport("user32.dll")]
    public static extern int GetSystemMetrics(int index);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool ShowWindow(IntPtr hwnd, int command);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool SetForegroundWindow(IntPtr hwnd);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdc, uint flags);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool PostMessageW(IntPtr hwnd, uint message, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern IntPtr SendMessageTimeoutW(IntPtr hwnd, uint message,
        IntPtr wParam, IntPtr lParam, uint flags, uint timeoutMilliseconds,
        out UIntPtr result);

    public static bool SendCommandWithTimeout(IntPtr hwnd, int functionCode,
        uint timeoutMilliseconds)
    {
        UIntPtr result;
        return SendMessageTimeoutW(hwnd, 0x0111, new IntPtr(functionCode),
            IntPtr.Zero, 0x0002, timeoutMilliseconds, out result) != IntPtr.Zero;
    }

    public static bool SendMouseWithTimeout(IntPtr hwnd, uint message, int x, int y,
        bool leftButtonDown, uint timeoutMilliseconds)
    {
        IntPtr point = new IntPtr(((y & 0xffff) << 16) | (x & 0xffff));
        UIntPtr result;
        return SendMessageTimeoutW(hwnd, message,
            leftButtonDown ? new IntPtr(1) : IntPtr.Zero, point,
            0x0002, timeoutMilliseconds, out result) != IntPtr.Zero;
    }

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetClassNameW(IntPtr hwnd, StringBuilder className, int maximum);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool SetWindowTextW(IntPtr hwnd, string text);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern int GetWindowTextW(IntPtr hwnd, StringBuilder text, int maximum);

    public static string ReadWindowText(IntPtr hwnd)
    {
        var text = new StringBuilder(4096);
        GetWindowTextW(hwnd, text, text.Capacity);
        return text.ToString();
    }

    public static IntPtr FindVisibleChildByClass(IntPtr parent, string expectedClass)
    {
        IntPtr match = IntPtr.Zero;
        EnumChildWindows(parent, delegate(IntPtr child, IntPtr unused)
        {
            if (!IsWindowVisible(child)) return true;
            var className = new StringBuilder(128);
            GetClassNameW(child, className, className.Capacity);
            if (!String.Equals(className.ToString(), expectedClass, StringComparison.Ordinal)) return true;
            match = child;
            return false;
        }, IntPtr.Zero);
        return match;
    }

    public static IntPtr FindAncestorByClass(IntPtr child, string expectedClass)
    {
        for (IntPtr current = GetParent(child); current != IntPtr.Zero; current = GetParent(current))
        {
            var className = new StringBuilder(128);
            GetClassNameW(current, className, className.Capacity);
            if (String.Equals(className.ToString(), expectedClass, StringComparison.Ordinal))
                return current;
        }
        return IntPtr.Zero;
    }

    public static IntPtr FindVisibleVerticalChildByClassAdjacentTo(IntPtr parent,
        string expectedClass, IntPtr reference)
    {
        IntPtr match = IntPtr.Zero;
        RECT referenceRect;
        if (!GetWindowRect(reference, out referenceRect)) return match;
        long bestDistance = long.MaxValue;
        EnumChildWindows(parent, delegate(IntPtr child, IntPtr unused)
        {
            if (!IsWindowVisible(child)) return true;
            var className = new StringBuilder(128);
            GetClassNameW(child, className, className.Capacity);
            RECT rect;
            if (!String.Equals(className.ToString(), expectedClass, StringComparison.Ordinal)
                || !GetWindowRect(child, out rect)) return true;
            if (rect.Bottom - rect.Top <= rect.Right - rect.Left) return true;
            if (rect.Bottom <= referenceRect.Top || rect.Top >= referenceRect.Bottom) return true;
            long centerX = ((long)rect.Left + rect.Right) / 2;
            long distance = Math.Min(Math.Abs(centerX - referenceRect.Left),
                Math.Abs(centerX - referenceRect.Right));
            if (distance >= bestDistance) return true;
            bestDistance = distance;
            match = child;
            return true;
        }, IntPtr.Zero);
        return match;
    }

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool ScreenToClient(IntPtr hwnd, ref POINT point);

    public static bool SendActivityBarClickWithTimeout(IntPtr hwnd, int slot,
        uint timeoutMilliseconds)
    {
        const int slotExtent = 42;
        int x = 21;
        int y = slot * slotExtent + slotExtent / 2;
        IntPtr point = new IntPtr((y << 16) | (x & 0xffff));
        UIntPtr result;
        if (SendMessageTimeoutW(hwnd, 0x0201, new IntPtr(1), point,
                0x0002, timeoutMilliseconds, out result) == IntPtr.Zero) return false;
        return SendMessageTimeoutW(hwnd, 0x0202, IntPtr.Zero, point,
            0x0002, timeoutMilliseconds, out result) != IntPtr.Zero;
    }

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool RedrawWindow(IntPtr hwnd, IntPtr updateRect, IntPtr updateRegion, uint flags);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool SetCursorPos(int x, int y);

    private delegate bool EnumChildProc(IntPtr hwnd, IntPtr parameter);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool EnumChildWindows(IntPtr parent, EnumChildProc callback, IntPtr parameter);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool IsWindowVisible(IntPtr hwnd);

    [DllImport("user32.dll")]
    private static extern IntPtr GetDC(IntPtr hwnd);

    [DllImport("user32.dll")]
    private static extern int ReleaseDC(IntPtr hwnd, IntPtr dc);

    [DllImport("gdi32.dll")]
    private static extern uint GetPixel(IntPtr dc, int x, int y);

    [DllImport("gdi32.dll")]
    private static extern IntPtr CreateCompatibleDC(IntPtr dc);

    [DllImport("gdi32.dll")]
    private static extern IntPtr CreateCompatibleBitmap(IntPtr dc, int width, int height);

    [DllImport("gdi32.dll")]
    private static extern IntPtr SelectObject(IntPtr dc, IntPtr value);

    [DllImport("gdi32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool DeleteObject(IntPtr value);

    [DllImport("gdi32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool DeleteDC(IntPtr dc);

    [DllImport("gdi32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool StretchBlt(IntPtr destination, int x, int y,
        int width, int height, IntPtr source, int sourceX, int sourceY,
        int sourceWidth, int sourceHeight, uint rasterOperation);

    public static ulong SampleWindowSignature(IntPtr hwnd)
    {
        RECT rect;
        if (!GetWindowRect(hwnd, out rect)) return 0;
        IntPtr screen = GetDC(IntPtr.Zero);
        if (screen == IntPtr.Zero) return 0;
        IntPtr sample = CreateCompatibleDC(screen);
        IntPtr bitmap = sample == IntPtr.Zero ? IntPtr.Zero
            : CreateCompatibleBitmap(screen, 11, 7);
        IntPtr previous = bitmap == IntPtr.Zero ? IntPtr.Zero
            : SelectObject(sample, bitmap);
        const ulong offset = 1469598103934665603UL;
        const ulong prime = 1099511628211UL;
        ulong hash = offset;
        try
        {
            if (sample == IntPtr.Zero || bitmap == IntPtr.Zero
                || previous == IntPtr.Zero || previous == new IntPtr(-1)
                || !StretchBlt(sample, 0, 0, 11, 7, screen,
                    rect.Left, rect.Top, rect.Right - rect.Left,
                    rect.Bottom - rect.Top, 0x00CC0020))
                return 0;
            // Read one captured miniature instead of issuing 77 independent
            // GetPixel calls against the live desktop. DirectComposition may
            // advance between live reads and manufacture a signature that no
            // single presented frame ever had.
            for (int y = 0; y < 7; ++y)
            {
                for (int x = 0; x < 11; ++x)
                {
                    hash ^= GetPixel(sample, x, y);
                    hash *= prime;
                }
            }
        }
        finally
        {
            if (previous != IntPtr.Zero && previous != new IntPtr(-1))
                SelectObject(sample, previous);
            if (bitmap != IntPtr.Zero) DeleteObject(bitmap);
            if (sample != IntPtr.Zero) DeleteDC(sample);
            ReleaseDC(IntPtr.Zero, screen);
        }
        hash ^= (uint)(rect.Right - rect.Left); hash *= prime;
        hash ^= (uint)(rect.Bottom - rect.Top); hash *= prime;
        return hash;
    }

    public static ulong VisibleChildLayoutSignature(IntPtr parent)
    {
        const ulong offset = 1469598103934665603UL;
        const ulong prime = 1099511628211UL;
        ulong hash = offset;
        EnumChildWindows(parent, delegate(IntPtr child, IntPtr unused)
        {
            if (!IsWindowVisible(child)) return true;
            RECT rect;
            if (!GetWindowRect(child, out rect)) return true;
            hash ^= (ulong)child.ToInt64(); hash *= prime;
            hash ^= (uint)rect.Left; hash *= prime;
            hash ^= (uint)rect.Top; hash *= prime;
            hash ^= (uint)rect.Right; hash *= prime;
            hash ^= (uint)rect.Bottom; hash *= prime;
            return true;
        }, IntPtr.Zero);
        return hash;
    }

    public sealed class DifferenceResult
    {
        public long DifferentPixels;
        public long TotalPixels;
        public Bitmap HeatMap;
    }

    public static DifferenceResult Difference(Bitmap screen, Bitmap printed, int tolerance)
    {
        if (screen.Width != printed.Width || screen.Height != printed.Height)
            throw new ArgumentException("Capture dimensions differ.");

        int width = screen.Width;
        int height = screen.Height;
        var heat = new Bitmap(width, height, PixelFormat.Format32bppArgb);
        var bounds = new Rectangle(0, 0, width, height);
        BitmapData a = screen.LockBits(bounds, ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
        BitmapData b = printed.LockBits(bounds, ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
        BitmapData d = heat.LockBits(bounds, ImageLockMode.WriteOnly, PixelFormat.Format32bppArgb);
        long different = 0;
        try
        {
            unsafe
            {
                for (int y = 0; y < height; ++y)
                {
                    byte* pa = (byte*)a.Scan0 + y * a.Stride;
                    byte* pb = (byte*)b.Scan0 + y * b.Stride;
                    byte* pd = (byte*)d.Scan0 + y * d.Stride;
                    for (int x = 0; x < width; ++x)
                    {
                        int i = x * 4;
                        bool changed = Math.Abs(pa[i] - pb[i]) > tolerance
                            || Math.Abs(pa[i + 1] - pb[i + 1]) > tolerance
                            || Math.Abs(pa[i + 2] - pb[i + 2]) > tolerance;
                        if (changed)
                        {
                            ++different;
                            pd[i] = 0; pd[i + 1] = 0; pd[i + 2] = 255; pd[i + 3] = 255;
                        }
                        else
                        {
                            byte gray = (byte)((pa[i] + pa[i + 1] + pa[i + 2]) / 9);
                            pd[i] = gray; pd[i + 1] = gray; pd[i + 2] = gray; pd[i + 3] = 255;
                        }
                    }
                }
            }
        }
        finally
        {
            screen.UnlockBits(a);
            printed.UnlockBits(b);
            heat.UnlockBits(d);
        }
        return new DifferenceResult { DifferentPixels = different,
            TotalPixels = (long)width * height, HeatMap = heat };
    }
}
'@ -CompilerOptions '/unsafe' -ReferencedAssemblies @($drawingAssembly, $drawingPrimitivesAssembly)

function Get-WindowRectangle {
	param([IntPtr]$Window)
	$rect = [SakuraFrameCoherenceNative+RECT]::new()
	if (-not [SakuraFrameCoherenceNative]::GetWindowRect($Window, [ref]$rect)) {
		throw "GetWindowRect failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
	}
	if ($rect.Right -le $rect.Left -or $rect.Bottom -le $rect.Top) {
		throw 'The target window has an empty rectangle.'
	}
	return $rect
}

function Get-CapturableWindowRectangle {
	param([IntPtr]$Window)
	$rect = [SakuraFrameCoherenceNative+RECT]::new()
	# GetWindowRect includes the transparent resize border on Windows 10/11.
	# Those pixels belong to windows behind Sakura and are not evidence of what
	# Sakura painted. DWMWA_EXTENDED_FRAME_BOUNDS is the visible DWM frame.
	$dwmResult = [SakuraFrameCoherenceNative]::DwmGetWindowAttribute(
		$Window, 9, [ref]$rect, [Runtime.InteropServices.Marshal]::SizeOf($rect))
	if ($dwmResult -ne 0 -or $rect.Right -le $rect.Left -or $rect.Bottom -le $rect.Top) {
		$rect = Get-WindowRectangle $Window
	}
	$virtualLeft = [SakuraFrameCoherenceNative]::GetSystemMetrics(76)
	$virtualTop = [SakuraFrameCoherenceNative]::GetSystemMetrics(77)
	$virtualRight = $virtualLeft + [SakuraFrameCoherenceNative]::GetSystemMetrics(78)
	$virtualBottom = $virtualTop + [SakuraFrameCoherenceNative]::GetSystemMetrics(79)
	$rect.Left = [Math]::Max($rect.Left, $virtualLeft)
	$rect.Top = [Math]::Max($rect.Top, $virtualTop)
	$rect.Right = [Math]::Min($rect.Right, $virtualRight)
	$rect.Bottom = [Math]::Min($rect.Bottom, $virtualBottom)
	if ($rect.Right -le $rect.Left -or $rect.Bottom -le $rect.Top) {
		throw 'The target window has no capturable pixels on the virtual screen.'
	}
	return $rect
}

function Assert-WindowUnoccluded {
	param([IntPtr]$Window, [SakuraFrameCoherenceNative+RECT]$Rectangle)
	$rootOwner = [SakuraFrameCoherenceNative]::GetAncestor($Window, 2)
	foreach ($xPart in 1..5) {
		foreach ($yPart in 1..5) {
			$point = [SakuraFrameCoherenceNative+POINT]::new()
			$point.X = $Rectangle.Left + [int](($Rectangle.Right - $Rectangle.Left) * $xPart / 6)
			$point.Y = $Rectangle.Top + [int](($Rectangle.Bottom - $Rectangle.Top) * $yPart / 6)
			$sample = [SakuraFrameCoherenceNative]::WindowFromPoint($point)
			$sampleRoot = [SakuraFrameCoherenceNative]::GetAncestor($sample, 2)
			if ($sampleRoot -ne $rootOwner) {
				throw "Window is occluded at ($($point.X), $($point.Y)); trial is invalid."
			}
		}
	}
}

function Capture-WindowPair {
	param([IntPtr]$Window, [string]$Prefix)
	$rect = Get-WindowRectangle $Window
	Assert-WindowUnoccluded $Window $rect
	# Observation must not mutate input state. The gesture driver parks or moves
	# the hardware cursor before capture; moving it here injects a real captured
	# WM_MOUSEMOVE into an otherwise deterministic synthesized resize.
	$width = $rect.Right - $rect.Left
	$height = $rect.Bottom - $rect.Top
	$screen = [Drawing.Bitmap]::new($width, $height, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
	$printed = [Drawing.Bitmap]::new($width, $height, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
	try {
		$screenGraphics = [Drawing.Graphics]::FromImage($screen)
		try {
			$screenGraphics.CopyFromScreen($rect.Left, $rect.Top, 0, 0,
				[Drawing.Size]::new($width, $height), [Drawing.CopyPixelOperation]::SourceCopy)
		}
		finally { $screenGraphics.Dispose() }

		$printGraphics = [Drawing.Graphics]::FromImage($printed)
		$printHdc = $printGraphics.GetHdc()
		try {
			if (-not [SakuraFrameCoherenceNative]::PrintWindow($Window, $printHdc, 2)) {
				throw "PrintWindow failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
			}
		}
		finally {
			$printGraphics.ReleaseHdc($printHdc)
			$printGraphics.Dispose()
		}

		$difference = [SakuraFrameCoherenceNative]::Difference($screen, $printed, $ChannelTolerance)
		try {
			$screenPath = Join-Path $OutputDirectory "$Prefix-screen.png"
			$printPath = Join-Path $OutputDirectory "$Prefix-printwindow.png"
			$heatPath = Join-Path $OutputDirectory "$Prefix-diff.png"
			$screen.Save($screenPath, [Drawing.Imaging.ImageFormat]::Png)
			$printed.Save($printPath, [Drawing.Imaging.ImageFormat]::Png)
			$difference.HeatMap.Save($heatPath, [Drawing.Imaging.ImageFormat]::Png)
			return [pscustomobject]@{
				prefix = $Prefix
				differentPixels = $difference.DifferentPixels
				totalPixels = $difference.TotalPixels
				percent = 100.0 * $difference.DifferentPixels / $difference.TotalPixels
				screen = $screenPath
				printWindow = $printPath
				heatMap = $heatPath
			}
		}
		finally { $difference.HeatMap.Dispose() }
	}
	finally {
		$screen.Dispose()
		$printed.Dispose()
	}
}

function Capture-PresentedScreen {
	param([IntPtr]$Window, [string]$Prefix)
	# GetWindowRect includes invisible resize borders and can extend beyond the
	# virtual desktop for a maximized window. CopyFromScreen leaves those pixels
	# undefined, which creates false changing strips at the bitmap edges.
	$rect = Get-CapturableWindowRectangle $Window
	Assert-WindowUnoccluded $Window $rect
	$width = $rect.Right - $rect.Left
	$height = $rect.Bottom - $rect.Top
	$screen = [Drawing.Bitmap]::new($width, $height, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
	try {
		$graphics = [Drawing.Graphics]::FromImage($screen)
		try {
			$graphics.CopyFromScreen($rect.Left, $rect.Top, 0, 0,
				[Drawing.Size]::new($width, $height), [Drawing.CopyPixelOperation]::SourceCopy)
		}
		finally { $graphics.Dispose() }
		$path = Join-Path $OutputDirectory "$Prefix-screen.png"
		$screen.Save($path, [Drawing.Imaging.ImageFormat]::Png)
		return $path
	}
	finally { $screen.Dispose() }
}

function Compare-SavedCaptures {
	param([string]$BeforePath, [string]$AfterPath, [string]$Prefix)
	$before = [Drawing.Bitmap]::new([IO.Path]::GetFullPath($BeforePath))
	$after = [Drawing.Bitmap]::new([IO.Path]::GetFullPath($AfterPath))
	try {
		$difference = [SakuraFrameCoherenceNative]::Difference($before, $after, $ChannelTolerance)
		try {
			$heatPath = Join-Path $OutputDirectory "$Prefix-diff.png"
			$difference.HeatMap.Save($heatPath, [Drawing.Imaging.ImageFormat]::Png)
			return [pscustomobject]@{
				prefix = $Prefix
				differentPixels = $difference.DifferentPixels
				totalPixels = $difference.TotalPixels
				percent = 100.0 * $difference.DifferentPixels / $difference.TotalPixels
				before = $BeforePath
				after = $AfterPath
				heatMap = $heatPath
			}
		}
		finally { $difference.HeatMap.Dispose() }
	}
	finally {
		$before.Dispose()
		$after.Dispose()
	}
}

function Save-CaptureCrop {
	param(
		[string]$SourcePath,
		[SakuraFrameCoherenceNative+RECT]$FrameRectangle,
		[SakuraFrameCoherenceNative+RECT]$CropRectangle,
		[string]$DestinationPath
	)
	$source = [Drawing.Bitmap]::new([IO.Path]::GetFullPath($SourcePath))
	try {
		$bounds = [Drawing.Rectangle]::new(
			$CropRectangle.Left - $FrameRectangle.Left,
			$CropRectangle.Top - $FrameRectangle.Top,
			$CropRectangle.Right - $CropRectangle.Left,
			$CropRectangle.Bottom - $CropRectangle.Top)
		if ($bounds.X -lt 0 -or $bounds.Y -lt 0 -or $bounds.Right -gt $source.Width `
			-or $bounds.Bottom -gt $source.Height -or $bounds.Width -le 0 -or $bounds.Height -le 0) {
			throw 'The requested surface crop is outside the captured top-level frame.'
		}
		$crop = $source.Clone($bounds, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
		try { $crop.Save($DestinationPath, [Drawing.Imaging.ImageFormat]::Png) }
		finally { $crop.Dispose() }
		return $DestinationPath
	}
	finally { $source.Dispose() }
}

function Wait-ForMainWindow {
	param([Diagnostics.Process]$Process, [int]$TimeoutMilliseconds)
	$timer = [Diagnostics.Stopwatch]::StartNew()
	do {
		$Process.Refresh()
		if ($Process.HasExited) { throw "sakura.exe exited before creating a window (exit $($Process.ExitCode))." }
		if ($Process.MainWindowHandle -ne [IntPtr]::Zero) { return $Process.MainWindowHandle }
		Start-Sleep -Milliseconds 20
	} while ($timer.ElapsedMilliseconds -lt $TimeoutMilliseconds)
	throw "Timed out waiting for the Sakura Editor window after $TimeoutMilliseconds ms."
}

function Wait-ForWindowQuiescence {
	param([IntPtr]$Window, [int]$TimeoutMilliseconds)
	$timer = [Diagnostics.Stopwatch]::StartNew()
	$lastSignature = $null
	$stableSinceMilliseconds = -1L
	# A handful of equal samples only proves that no paint happened for a few
	# message-loop turns. SCM and Search can publish worker results hundreds of
	# milliseconds later, so require a real quiet interval before treating their
	# current pixels as the baseline for a resize trial.
	$requiredQuietMilliseconds = 300
	do {
		$signature = [SakuraFrameCoherenceNative]::SampleWindowSignature($Window)
		if ($signature -ne 0 -and $signature -eq $lastSignature) {
			if ($stableSinceMilliseconds -lt 0) {
				$stableSinceMilliseconds = $timer.ElapsedMilliseconds
			}
			elseif ($timer.ElapsedMilliseconds - $stableSinceMilliseconds -ge $requiredQuietMilliseconds) {
				return
			}
		}
		else {
			$lastSignature = $signature
			$stableSinceMilliseconds = $timer.ElapsedMilliseconds
		}
		Start-Sleep -Milliseconds 20
	} while ($timer.ElapsedMilliseconds -lt $TimeoutMilliseconds)
	throw "Window did not remain visually stable for $requiredQuietMilliseconds ms within $TimeoutMilliseconds ms."
}

function Wait-ForChildLayoutChange {
	param([IntPtr]$Window, [UInt64]$Before, [int]$TimeoutMilliseconds)
	$timer = [Diagnostics.Stopwatch]::StartNew()
	do {
		$current = [SakuraFrameCoherenceNative]::VisibleChildLayoutSignature($Window)
		if ($current -ne $Before) { return $current }
		Start-Sleep -Milliseconds 10
	} while ($timer.ElapsedMilliseconds -lt $TimeoutMilliseconds)
	throw "The command did not change any visible child layout within $TimeoutMilliseconds ms."
}

function Remove-TestProfile {
	param([string]$Name)
	if (-not $Name.StartsWith('codex-render-', [StringComparison]::Ordinal)) {
		throw "Refusing to remove profile without codex-render- prefix: $Name"
	}
	$profileRoot = [IO.Path]::GetFullPath((Join-Path $env:APPDATA 'sakura'))
	$profile = [IO.Path]::GetFullPath((Join-Path $profileRoot $Name))
	if (-not $profile.StartsWith($profileRoot + [IO.Path]::DirectorySeparatorChar,
		[StringComparison]::OrdinalIgnoreCase)) {
		throw "Resolved profile escaped the Sakura profile root: $profile"
	}
	if (-not [IO.Directory]::Exists($profile)) { return }
	$delayMilliseconds = 25
	for ($attempt = 1; $attempt -le 6; ++$attempt) {
		try {
			[IO.Directory]::Delete($profile, $true)
			return
		}
		catch [IO.IOException] {
			if ($attempt -eq 6) { throw }
			[Threading.Thread]::Sleep($delayMilliseconds)
			$delayMilliseconds = [Math]::Min(400, $delayMilliseconds * 2)
		}
		catch [UnauthorizedAccessException] {
			if ($attempt -eq 6) { throw }
			[Threading.Thread]::Sleep($delayMilliseconds)
			$delayMilliseconds = [Math]::Min(400, $delayMilliseconds * 2)
		}
	}
}

$resolvedExecutable = [IO.Path]::GetFullPath($Executable)
if (-not [IO.File]::Exists($resolvedExecutable)) { throw "Executable not found: $resolvedExecutable" }
if ($Gesture -eq 'Command' -and $FunctionCode -le 0) {
	throw 'Command gesture requires a positive -FunctionCode.'
}
[IO.Directory]::CreateDirectory([IO.Path]::GetFullPath($OutputDirectory)) | Out-Null
Remove-TestProfile $ProfileName

$argumentList = @("-PROF=$ProfileName")
if (-not [string]::IsNullOrWhiteSpace($WorkspaceFolder)) {
	$resolvedWorkspaceFolder = [IO.Path]::GetFullPath($WorkspaceFolder)
	if (-not [IO.Directory]::Exists($resolvedWorkspaceFolder)) {
		throw "Workspace folder not found: $resolvedWorkspaceFolder"
	}
	$argumentList += "-FOLDER=$resolvedWorkspaceFolder"
} else {
	$resolvedWorkspaceFolder = $null
}
if (-not [string]::IsNullOrWhiteSpace($DocumentPath)) {
	$argumentList += [IO.Path]::GetFullPath($DocumentPath)
}
$startInfo = [Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = $resolvedExecutable
$startInfo.UseShellExecute = $false
foreach ($argument in $argumentList) { [void]$startInfo.ArgumentList.Add($argument) }
$process = [Diagnostics.Process]::Start($startInfo)

$results = [Collections.Generic.List[object]]::new()
$noiseFloors = [Collections.Generic.List[object]]::new()
$surfaceResults = [Collections.Generic.List[object]]::new()
$surfaceNoiseFloors = [Collections.Generic.List[object]]::new()
$screenStabilityResults = [Collections.Generic.List[object]]::new()
$surfaceScreenStabilityResults = [Collections.Generic.List[object]]::new()
$presentedScreenResults = [Collections.Generic.List[object]]::new()
$presentedSurfaceResults = [Collections.Generic.List[object]]::new()
try {
	$window = Wait-ForMainWindow $process $ReadyTimeoutMilliseconds
	# CopyFromScreen has no stable meaning outside the physical desktop. A
	# maximized Win32 frame commonly extends its resize border past that boundary,
	# so place this throwaway test window wholly inside the primary display.
	$launchRect = Get-WindowRectangle $window
	$screenWidth = [SakuraFrameCoherenceNative]::GetSystemMetrics(0)
	$screenHeight = [SakuraFrameCoherenceNative]::GetSystemMetrics(1)
	$testWidth = [Math]::Max(640, [Math]::Min($launchRect.Right - $launchRect.Left, $screenWidth - 80))
	$testHeight = [Math]::Max(480, [Math]::Min($launchRect.Bottom - $launchRect.Top, $screenHeight - 80))
	[void][SakuraFrameCoherenceNative]::ShowWindow($window, 9)
	if (-not [SakuraFrameCoherenceNative]::SetWindowPos(
		$window, [IntPtr]::Zero, 40, 40, $testWidth, $testHeight, 0x0014)) {
		throw 'Failed to place the test window inside the physical display.'
	}
	# Screen capture is only meaningful while the tested top-level window owns
	# the foreground z-order. Keep this isolated throwaway-profile process above
	# the driver for the bounded trial, then remove the topmost state in finally.
	[void][SakuraFrameCoherenceNative]::SetForegroundWindow($window)
	[void][SakuraFrameCoherenceNative]::SetWindowPos(
		$window, [IntPtr](-1), 0, 0, 0, 0, 0x0013)
	$parkingRect = Get-WindowRectangle $window
	# Park over the stable editor body between trials. During a Side Bar drag the
	# hardware cursor follows the synthesized coordinates and remains on the sash
	# through both captures, keeping native hover state identical.
	$parkingX = [Convert]::ToInt32($parkingRect.Left + 3 * ($parkingRect.Right - $parkingRect.Left) / 4)
	$parkingY = [Convert]::ToInt32($parkingRect.Top + ($parkingRect.Bottom - $parkingRect.Top) / 2)
	[void][SakuraFrameCoherenceNative]::SetCursorPos($parkingX, $parkingY)
	if ($ActivityBarPage -eq 'Default') {
		Wait-ForWindowQuiescence $window $ReadyTimeoutMilliseconds
	}
	$surfaceWindow = [IntPtr]::Zero
	$surfaceHostWindow = [IntPtr]::Zero
	if ($ActivityBarPage -ne 'Default') {
		$surfaceClass = switch ($ActivityBarPage) {
			'Explorer' { 'SakuraNativeExplorerTool' }
			'Search' { 'SakuraNativeSearchTool' }
			'SourceControl' { 'SakuraNativeScmTool' }
		}
		$surfaceWindow = [SakuraFrameCoherenceNative]::FindVisibleChildByClass(
			$window, $surfaceClass)
		$activityBar = [SakuraFrameCoherenceNative]::FindVisibleChildByClass(
			$window, 'SakuraWorkbenchActivityBar')
		if ($activityBar -eq [IntPtr]::Zero) {
			throw 'The visible SakuraWorkbenchActivityBar child was not found.'
		}
		$slot = switch ($ActivityBarPage) {
			'Explorer' { 0 }
			'Search' { 1 }
			'SourceControl' { 2 }
		}
		if ($surfaceWindow -eq [IntPtr]::Zero) {
			$beforePage = [SakuraFrameCoherenceNative]::VisibleChildLayoutSignature($window)
			if (-not [SakuraFrameCoherenceNative]::SendActivityBarClickWithTimeout(
				$activityBar, $slot, [uint32]$ReadyTimeoutMilliseconds)) {
				throw "Activity Bar click failed for $ActivityBarPage."
			}
			[void](Wait-ForChildLayoutChange $window $beforePage $ReadyTimeoutMilliseconds)
			$surfaceWindow = [SakuraFrameCoherenceNative]::FindVisibleChildByClass(
				$window, $surfaceClass)
		}
		if ($surfaceWindow -eq [IntPtr]::Zero) {
			throw "The activated $ActivityBarPage surface child was not found."
		}
		$surfaceHostWindow = [SakuraFrameCoherenceNative]::FindAncestorByClass(
			$surfaceWindow, 'SakuraWorkbenchPanelHost')
		if ($surfaceHostWindow -eq [IntPtr]::Zero) {
			throw "The owning Workbench Part for $ActivityBarPage was not found."
		}
		if ($ActivityBarPage -eq 'Search' -and -not [string]::IsNullOrWhiteSpace($SearchQuery)) {
			$queryWindow = [SakuraFrameCoherenceNative]::FindVisibleChildByClass($surfaceWindow, 'Edit')
			if ($queryWindow -eq [IntPtr]::Zero) { throw 'The Search query control was not found.' }
			if (-not [SakuraFrameCoherenceNative]::SetWindowTextW($queryWindow, $SearchQuery)) {
				throw "SetWindowTextW failed for the Search query: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
			}
			if ([SakuraFrameCoherenceNative]::ReadWindowText($queryWindow) -ne $SearchQuery) {
				throw 'The Search query control did not retain the requested text.'
			}
			Wait-ForWindowQuiescence $surfaceWindow $ReadyTimeoutMilliseconds
		}
		# The first SCM/Search publication is asynchronous and can arrive after a
		# visually quiet startup interval. Do not let that legitimate initial
		# population become the "settled" half of the first resize comparison.
		Start-Sleep -Milliseconds 1200
		Wait-ForWindowQuiescence $surfaceWindow $ReadyTimeoutMilliseconds
	}
	if ($Gesture -eq 'SideBarResize' -and $surfaceWindow -ne [IntPtr]::Zero) {
		# A new profile may inherit the legacy shared Side Bar extent from the
		# control process. Normalize the test fixture before taking any baseline so
		# earlier trials cannot turn a later run into a different-size experiment.
		$normalizationTrace = [Collections.Generic.List[string]]::new()
		for ($attempt = 1; $attempt -le 4; ++$attempt) {
			# The Part host is layout authority. A retained child surface may keep a
			# larger buffer/old HWND rectangle while its parent clips it; treating that
			# child rectangle as the visible Side Bar width manufactures a bad drag.
			$surfaceRect = Get-WindowRectangle $surfaceHostWindow
			$surfaceWidth = $surfaceRect.Right - $surfaceRect.Left
			if ([Math]::Abs($surfaceWidth - 320) -le 1) {
				$normalizationTrace.Add("attempt=$attempt width=$surfaceWidth converged")
				break
			}
			$sash = [SakuraFrameCoherenceNative]::FindVisibleVerticalChildByClassAdjacentTo(
				$window, 'SakuraWorkbenchPanelSash', $surfaceHostWindow)
			if ($sash -eq [IntPtr]::Zero) { throw 'The visible Primary Side Bar sash was not found.' }
			$sashRect = Get-WindowRectangle $sash
			$origin = [SakuraFrameCoherenceNative+POINT]::new()
			$originScreenX = [int](($sashRect.Left + $sashRect.Right) / 2)
			$originScreenY = [int](($sashRect.Top + $sashRect.Bottom) / 2)
			$origin.X = $originScreenX
			$origin.Y = $originScreenY
			if (-not [SakuraFrameCoherenceNative]::ScreenToClient($window, [ref]$origin)) {
				throw 'ScreenToClient failed while normalizing the Side Bar.'
			}
			$targetX = $origin.X + 320 - $surfaceWidth
			$targetScreenX = $originScreenX + $targetX - $origin.X
			$normalizationTrace.Add(
				"attempt=$attempt width=$surfaceWidth surface=$($surfaceRect.Left)..$($surfaceRect.Right) sash=$($sashRect.Left)..$($sashRect.Right) origin=$($origin.X) target=$targetX")
			[void][SakuraFrameCoherenceNative]::SetCursorPos($originScreenX, $originScreenY)
			$normalizeDown = [SakuraFrameCoherenceNative]::SendMouseWithTimeout(
				$window, 0x0201, $origin.X, $origin.Y, $true, [uint32]$ReadyTimeoutMilliseconds)
			[void][SakuraFrameCoherenceNative]::SetCursorPos($targetScreenX, $originScreenY)
			$normalizeMove = [SakuraFrameCoherenceNative]::SendMouseWithTimeout(
				$window, 0x0200, $targetX, $origin.Y, $true, [uint32]$ReadyTimeoutMilliseconds)
			$dragRect = Get-WindowRectangle $surfaceHostWindow
			$normalizationTrace.Add(
				"attempt=$attempt dragWidth=$($dragRect.Right - $dragRect.Left)")
			$normalizeUp = [SakuraFrameCoherenceNative]::SendMouseWithTimeout(
				$window, 0x0202, $targetX, $origin.Y, $false, [uint32]$ReadyTimeoutMilliseconds)
			[void][SakuraFrameCoherenceNative]::SetCursorPos($parkingX, $parkingY)
			if (-not $normalizeDown -or -not $normalizeMove -or -not $normalizeUp) {
				throw 'Failed to normalize the Primary Side Bar width.'
			}
			Wait-ForWindowQuiescence $window $ReadyTimeoutMilliseconds
		}
		$normalizedRect = Get-WindowRectangle $surfaceHostWindow
		$normalizedWidth = $normalizedRect.Right - $normalizedRect.Left
		if ([Math]::Abs($normalizedWidth - 320) -gt 1) {
			[void](Capture-PresentedScreen $window 'normalization-failed')
			throw "Primary Side Bar normalization did not converge; measured width $normalizedWidth; $($normalizationTrace -join '; ')."
		}
	}
	$initial = Get-WindowRectangle $window
	$baseWidth = $initial.Right - $initial.Left
	$baseHeight = $initial.Bottom - $initial.Top
	for ($trial = 1; $trial -le $Trials; ++$trial) {
		if ($Gesture -eq 'Resize') {
			$delta = if (($trial % 2) -eq 0) { 173 } else { -137 }
			$width = [Math]::Max(640, $baseWidth + $delta)
			if (-not [SakuraFrameCoherenceNative]::SetWindowPos(
				$window, [IntPtr]::Zero, $initial.Left, $initial.Top, $width, $baseHeight, 0x0014)) {
				throw "SetWindowPos failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
			}
			$applied = Get-WindowRectangle $window
			if (($applied.Right - $applied.Left) -ne $width) {
				throw "Resize gesture was not applied; expected width $width."
			}
		}
		elseif ($Gesture -eq 'SideBarResize') {
			$sash = [SakuraFrameCoherenceNative]::FindVisibleVerticalChildByClassAdjacentTo(
				$window, 'SakuraWorkbenchPanelSash', $surfaceHostWindow)
			if ($sash -eq [IntPtr]::Zero) { throw 'The visible Primary Side Bar sash was not found.' }
			$sashRect = Get-WindowRectangle $sash
			$origin = [SakuraFrameCoherenceNative+POINT]::new()
			$originScreenX = [int](($sashRect.Left + $sashRect.Right) / 2)
			$originScreenY = [int](($sashRect.Top + $sashRect.Bottom) / 2)
			$origin.X = $originScreenX
			$origin.Y = $originScreenY
			if (-not [SakuraFrameCoherenceNative]::ScreenToClient($window, [ref]$origin)) {
				throw "ScreenToClient failed for the Side Bar sash: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
			}
			$targetX = $origin.X + $(if (($trial % 2) -eq 0) { -72 } else { 72 })
			$targetScreenX = $originScreenX + $targetX - $origin.X
			$dragStarted = $false
			try {
				[void][SakuraFrameCoherenceNative]::SetCursorPos($originScreenX, $originScreenY)
				if (-not [SakuraFrameCoherenceNative]::SendMouseWithTimeout(
					$window, 0x0201, $origin.X, $origin.Y, $true, [uint32]$ReadyTimeoutMilliseconds)) {
					throw 'WM_LBUTTONDOWN failed for the Side Bar sash.'
				}
				$dragStarted = $true
				[void][SakuraFrameCoherenceNative]::SetCursorPos($targetScreenX, $originScreenY)
				if (-not [SakuraFrameCoherenceNative]::SendMouseWithTimeout(
					$window, 0x0200, $targetX, $origin.Y, $true, [uint32]$ReadyTimeoutMilliseconds)) {
					throw 'WM_MOUSEMOVE failed for the Side Bar sash.'
				}
				if ($PresentedScreenOnly) {
					$beforeFrameRect = Get-CapturableWindowRectangle $window
					$beforeSurfaceRect = Get-WindowRectangle $surfaceWindow
					$beforeScreen = Capture-PresentedScreen $window ("trial-{0:D3}-immediate" -f $trial)
				}
			}
			finally {
				if ($dragStarted) {
					[void][SakuraFrameCoherenceNative]::SendMouseWithTimeout(
						$window, 0x0202, $targetX, $origin.Y, $false, [uint32]$ReadyTimeoutMilliseconds)
				}
			}
			if ($PresentedScreenOnly) {
				Wait-ForWindowQuiescence $window $ReadyTimeoutMilliseconds
				$afterScreen = Capture-PresentedScreen $window ("trial-{0:D3}-settled" -f $trial)
				$presentedScreenResults.Add((Compare-SavedCaptures `
					-BeforePath $beforeScreen -AfterPath $afterScreen `
					-Prefix ("trial-{0:D3}-presented-stability" -f $trial)))
				if ($surfaceWindow -ne [IntPtr]::Zero) {
					$afterFrameRect = Get-CapturableWindowRectangle $window
					$afterSurfaceRect = Get-WindowRectangle $surfaceWindow
					$fixedSurfaceRect = [SakuraFrameCoherenceNative+RECT]::new()
					$fixedSurfaceRect.Left = [Math]::Max($beforeSurfaceRect.Left, $afterSurfaceRect.Left)
					$fixedSurfaceRect.Top = [Math]::Max($beforeSurfaceRect.Top, $afterSurfaceRect.Top)
					$fixedSurfaceRect.Right = [Math]::Min($beforeSurfaceRect.Right, $afterSurfaceRect.Right)
					$fixedSurfaceRect.Bottom = [Math]::Min($beforeSurfaceRect.Bottom, $afterSurfaceRect.Bottom)
					$beforeSurface = Save-CaptureCrop $beforeScreen $beforeFrameRect $fixedSurfaceRect `
						(Join-Path $OutputDirectory ("trial-{0:D3}-surface-immediate-screen.png" -f $trial))
					$afterSurface = Save-CaptureCrop $afterScreen $afterFrameRect $fixedSurfaceRect `
						(Join-Path $OutputDirectory ("trial-{0:D3}-surface-settled-screen.png" -f $trial))
					$presentedSurfaceResults.Add((Compare-SavedCaptures `
						-BeforePath $beforeSurface -AfterPath $afterSurface `
						-Prefix ("trial-{0:D3}-surface-presented-stability" -f $trial)))
				}
				[void][SakuraFrameCoherenceNative]::SetCursorPos($parkingX, $parkingY)
				continue
			}
		}
		else {
			$beforeLayout = [SakuraFrameCoherenceNative]::VisibleChildLayoutSignature($window)
			if (-not [SakuraFrameCoherenceNative]::SendCommandWithTimeout(
				$window, $FunctionCode, [uint32]$ReadyTimeoutMilliseconds)) {
				throw "SendMessageTimeout(WM_COMMAND) failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
			}
			[void](Wait-ForChildLayoutChange $window $beforeLayout $ReadyTimeoutMilliseconds)
		}
		if ($PresentedScreenOnly) {
			$beforeScreen = Capture-PresentedScreen $window ("trial-{0:D3}-immediate" -f $trial)
			$beforeSurface = if ($surfaceWindow -ne [IntPtr]::Zero) {
				Capture-PresentedScreen $surfaceWindow ("trial-{0:D3}-surface-immediate" -f $trial)
			} else { $null }
			Wait-ForWindowQuiescence $window $ReadyTimeoutMilliseconds
			$afterScreen = Capture-PresentedScreen $window ("trial-{0:D3}-settled" -f $trial)
			$presentedScreenResults.Add((Compare-SavedCaptures `
				-BeforePath $beforeScreen -AfterPath $afterScreen `
				-Prefix ("trial-{0:D3}-presented-stability" -f $trial)))
			if ($surfaceWindow -ne [IntPtr]::Zero) {
				$afterSurface = Capture-PresentedScreen $surfaceWindow ("trial-{0:D3}-surface-settled" -f $trial)
				$presentedSurfaceResults.Add((Compare-SavedCaptures `
					-BeforePath $beforeSurface -AfterPath $afterSurface `
					-Prefix ("trial-{0:D3}-surface-presented-stability" -f $trial)))
			}
			continue
		}
		$measurement = Capture-WindowPair $window ("trial-{0:D3}" -f $trial)
		$results.Add($measurement)
		if ($surfaceWindow -ne [IntPtr]::Zero) {
			$surfaceResults.Add((Capture-WindowPair $surfaceWindow ("trial-{0:D3}-surface" -f $trial)))
		}
		# PrintWindow has stable geometry-dependent differences from the composed
		# screen. Measure the full-redraw floor at the exact same window size and
		# semantic state; a floor from another resize direction is not comparable.
		if (-not [SakuraFrameCoherenceNative]::RedrawWindow(
			$window, [IntPtr]::Zero, [IntPtr]::Zero, 0x0585)) {
			throw "RedrawWindow noise-floor probe failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
		}
		Wait-ForWindowQuiescence $window $ReadyTimeoutMilliseconds
		$noiseFloor = Capture-WindowPair $window ("trial-{0:D3}-noise-floor" -f $trial)
		$noiseFloors.Add($noiseFloor)
		$screenStabilityPrefix = "trial-{0:D3}-screen-stability" -f $trial
		$screenStabilityResults.Add((Compare-SavedCaptures `
			-BeforePath $measurement.screen -AfterPath $noiseFloor.screen -Prefix $screenStabilityPrefix))
		if ($surfaceWindow -ne [IntPtr]::Zero) {
			$surfaceNoiseFloors.Add((Capture-WindowPair $surfaceWindow (
				"trial-{0:D3}-surface-noise-floor" -f $trial)))
			$surfaceMeasurement = $surfaceResults[$surfaceResults.Count - 1]
			$surfaceNoiseFloor = $surfaceNoiseFloors[$surfaceNoiseFloors.Count - 1]
			$surfaceMeasurement | Add-Member -NotePropertyName excessOverNoiseFloorPercent -NotePropertyValue (
				[Math]::Max(0.0, $surfaceMeasurement.percent - $surfaceNoiseFloor.percent))
			$surfaceMeasurement | Add-Member -NotePropertyName noiseFloorPercent -NotePropertyValue $surfaceNoiseFloor.percent
			$surfaceScreenStabilityPrefix = "trial-{0:D3}-surface-screen-stability" -f $trial
			$surfaceScreenStabilityResults.Add((Compare-SavedCaptures `
				-BeforePath $surfaceMeasurement.screen -AfterPath $surfaceNoiseFloor.screen `
				-Prefix $surfaceScreenStabilityPrefix))
		}
		$measurement | Add-Member -NotePropertyName excessOverNoiseFloorPercent -NotePropertyValue (
			[Math]::Max(0.0, $measurement.percent - $noiseFloor.percent))
		$measurement | Add-Member -NotePropertyName noiseFloorPercent -NotePropertyValue $noiseFloor.percent
	}
	if ($PresentedScreenOnly) {
		# The surface crop localizes a failure; it must not silently tighten the
		# global pixel budget merely because its denominator is smaller. Applying
		# one absolute budget still rejects a delayed 1,094-pixel text repaint while
		# tolerating the same sub-threshold hover transition as the full frame.
		$allowedPresentedDifferentPixels = [Math]::Floor(
			$presentedScreenResults[0].totalPixels * $AllowedExcessPercent / 100.0)
		$failedPresented = @($presentedScreenResults | Where-Object { $_.percent -gt $AllowedExcessPercent })
		$failedPresentedSurface = @($presentedSurfaceResults | Where-Object {
			$_.differentPixels -gt $allowedPresentedDifferentPixels
		})
		$summary = [ordered]@{
			executable = $resolvedExecutable
			profile = $ProfileName
			workspaceFolder = $resolvedWorkspaceFolder
			gesture = $Gesture
			activityBarPage = $ActivityBarPage
			trials = $Trials
			channelTolerance = $ChannelTolerance
			allowedExcessPercent = $AllowedExcessPercent
			allowedDifferentPixels = $allowedPresentedDifferentPixels
			presentedScreenMaximumPercent = (@($presentedScreenResults | ForEach-Object percent | Sort-Object))[-1]
			failedPresentedScreenTrialCount = $failedPresented.Count
			presentedSurfaceMaximumPercent = if ($presentedSurfaceResults.Count -ne 0) {
				(@($presentedSurfaceResults | ForEach-Object percent | Sort-Object))[-1]
			} else { $null }
			failedPresentedSurfaceTrialCount = $failedPresentedSurface.Count
			presentedScreenMeasurements = @($presentedScreenResults)
			presentedSurfaceMeasurements = @($presentedSurfaceResults)
		}
		$summaryPath = Join-Path $OutputDirectory 'summary.json'
		$summary | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $summaryPath -Encoding utf8
		$summary | ConvertTo-Json -Depth 6
		if ($FailOnExcess -and ($failedPresented.Count -ne 0 -or $failedPresentedSurface.Count -ne 0)) { exit 2 }
		return
	}
	$percentages = @($results | ForEach-Object percent | Sort-Object)
	$excesses = @($results | ForEach-Object excessOverNoiseFloorPercent | Sort-Object)
	$failedMeasurements = @($results | Where-Object {
		$_.excessOverNoiseFloorPercent -gt $AllowedExcessPercent
	})
	$failedSurfaceMeasurements = @($surfaceResults | Where-Object {
		$_.excessOverNoiseFloorPercent -gt $AllowedExcessPercent
	})
	$failedScreenStability = @($screenStabilityResults | Where-Object {
		$_.percent -gt $AllowedExcessPercent
	})
	$failedSurfaceScreenStability = @($surfaceScreenStabilityResults | Where-Object {
		$_.percent -gt $AllowedExcessPercent
	})
	$summary = [ordered]@{
		executable = $resolvedExecutable
		profile = $ProfileName
		workspaceFolder = $resolvedWorkspaceFolder
		gesture = $Gesture
		activityBarPage = $ActivityBarPage
		searchQuery = if ($ActivityBarPage -eq 'Search') { $SearchQuery } else { $null }
		functionCode = $FunctionCode
		trials = $Trials
		channelTolerance = $ChannelTolerance
		allowedExcessPercent = $AllowedExcessPercent
		minimumPercent = $percentages[0]
		medianPercent = $percentages[[int][Math]::Floor(($percentages.Count - 1) / 2)]
		maximumPercent = $percentages[-1]
		minimumNoiseFloorPercent = (@($noiseFloors | ForEach-Object percent | Sort-Object))[0]
		maximumNoiseFloorPercent = (@($noiseFloors | ForEach-Object percent | Sort-Object))[-1]
		maximumExcessOverNoiseFloorPercent = $excesses[-1]
		failedTrialCount = $failedMeasurements.Count
		surfaceMaximumExcessOverNoiseFloorPercent = if ($surfaceResults.Count -ne 0) {
			(@($surfaceResults | ForEach-Object excessOverNoiseFloorPercent | Sort-Object))[-1]
		} else { $null }
		failedSurfaceTrialCount = $failedSurfaceMeasurements.Count
		screenStabilityMaximumPercent = (@($screenStabilityResults | ForEach-Object percent | Sort-Object))[-1]
		failedScreenStabilityTrialCount = $failedScreenStability.Count
		surfaceScreenStabilityMaximumPercent = if ($surfaceScreenStabilityResults.Count -ne 0) {
			(@($surfaceScreenStabilityResults | ForEach-Object percent | Sort-Object))[-1]
		} else { $null }
		failedSurfaceScreenStabilityTrialCount = $failedSurfaceScreenStability.Count
		measurements = @($results)
		noiseFloors = @($noiseFloors)
		surfaceMeasurements = @($surfaceResults)
		surfaceNoiseFloors = @($surfaceNoiseFloors)
		screenStabilityMeasurements = @($screenStabilityResults)
		surfaceScreenStabilityMeasurements = @($surfaceScreenStabilityResults)
	}
	$summaryPath = Join-Path $OutputDirectory 'summary.json'
	$summary | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $summaryPath -Encoding utf8
	$summary | ConvertTo-Json -Depth 6
	if ($FailOnExcess -and ($failedMeasurements.Count -ne 0 -or $failedSurfaceMeasurements.Count -ne 0 `
		-or $failedScreenStability.Count -ne 0 -or $failedSurfaceScreenStability.Count -ne 0)) { exit 2 }
}
finally {
	if ($process -and -not $process.HasExited -and $process.MainWindowHandle -ne [IntPtr]::Zero) {
		[void][SakuraFrameCoherenceNative]::SetWindowPos(
			$process.MainWindowHandle, [IntPtr](-2), 0, 0, 0, 0, 0x0013)
	}
	if ($process -and -not $process.HasExited) {
		[void][SakuraFrameCoherenceNative]::PostMessageW(
			$process.MainWindowHandle, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
		if (-not $process.WaitForExit(2000)) {
			$process.Kill($true)
			[void]$process.WaitForExit(2000)
		}
	}
	if ($process) { $process.Dispose() }
	Remove-TestProfile $ProfileName
}
