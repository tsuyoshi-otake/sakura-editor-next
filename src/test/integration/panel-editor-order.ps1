param(
    [string]$Executable = (Join-Path $PSScriptRoot '../../../x64/Debug/sakura.exe'),
    [ValidateRange(1, 20)][int]$Trials = 3,
    [string]$OutputDirectory = (Join-Path $env:USERPROFILE "tmp/sakura-panel-editor-order-$PID")
)

# Visible native integration test; use only disposable profiles. Pixel verification
# is separate: follow .claude/skills/stale-pixel-verification/SKILL.md.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$timer = [Diagnostics.Stopwatch]::StartNew()
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$Executable = [IO.Path]::GetFullPath($Executable)
$document = Join-Path $repository 'CLAUDE.md'
if (-not [IO.File]::Exists($Executable)) { throw "Executable not found: $Executable" }
[IO.Directory]::CreateDirectory([IO.Path]::GetFullPath($OutputDirectory)) | Out-Null

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public static class PanelEditorOrderNative {
    public struct Rect { public int Left, Top, Right, Bottom; }
    public sealed class Child {
        public long Handle;
        public string Class;
        public int Id;
        public bool Visible;
        public Rect Bounds;
    }
    private delegate bool EnumCallback(IntPtr window, IntPtr parameter);
    [DllImport("user32.dll")] private static extern bool EnumChildWindows(IntPtr window, EnumCallback callback, IntPtr parameter);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetClassNameW(IntPtr window, StringBuilder text, int capacity);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetWindowTextW(IntPtr window, StringBuilder text, int capacity);
    [DllImport("user32.dll")] private static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] private static extern int GetDlgCtrlID(IntPtr window);
    [DllImport("user32.dll")] private static extern bool GetWindowRect(IntPtr window, out Rect bounds);
    [DllImport("user32.dll")] public static extern IntPtr GetParent(IntPtr window);
    [DllImport("user32.dll")] private static extern IntPtr SendMessageTimeoutW(IntPtr window, uint message, IntPtr wParam, IntPtr lParam, uint flags, uint timeout, out UIntPtr result);
    [DllImport("user32.dll")] private static extern bool PostMessageW(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);
    [DllImport("kernel32.dll")] private static extern IntPtr GlobalAlloc(uint flags, UIntPtr bytes);
    [DllImport("kernel32.dll")] private static extern IntPtr GlobalLock(IntPtr handle);
    [DllImport("kernel32.dll")] private static extern bool GlobalUnlock(IntPtr handle);
    [DllImport("kernel32.dll")] private static extern IntPtr GlobalFree(IntPtr handle);

    public static Child[] Children(IntPtr window) {
        var children = new List<Child>();
        EnumChildWindows(window, (child, unused) => {
            var name = new StringBuilder(256);
            GetClassNameW(child, name, name.Capacity);
            Rect bounds;
            if (!GetWindowRect(child, out bounds)) throw new InvalidOperationException("GetWindowRect failed");
            children.Add(new Child { Handle = child.ToInt64(), Class = name.ToString(),
                Id = GetDlgCtrlID(child), Visible = IsWindowVisible(child), Bounds = bounds });
            return true;
        }, IntPtr.Zero);
        return children.ToArray();
    }

    public static string Title(IntPtr window) {
        var text = new StringBuilder(4096);
        GetWindowTextW(window, text, text.Capacity);
        return text.ToString();
    }

    public static void Command(IntPtr window, int command) {
        UIntPtr result;
        if (SendMessageTimeoutW(window, 0x0111, new IntPtr(command), IntPtr.Zero, 2, 3000, out result) == IntPtr.Zero)
            throw new InvalidOperationException("WM_COMMAND failed or timed out");
    }

    public static void RefreshEditorSnapshot(IntPtr window) {
        UIntPtr result;
        // MYWM_EDITOR_CORE_CHANGED is a presentation refresh, not a reveal request.
        if (SendMessageTimeoutW(window, 0x8000 + 238, IntPtr.Zero, IntPtr.Zero, 2, 3000, out result) == IntPtr.Zero)
            throw new InvalidOperationException("Editor snapshot refresh failed or timed out");
    }

    public static void OpenFile(IntPtr window, string path) {
        // WM_DROPFILES transfers ownership to the editor's ordinary file-open path.
        var bytes = Encoding.Unicode.GetBytes(path + "\0\0");
        var allocation = GlobalAlloc(0x42, (UIntPtr)(20 + bytes.Length));
        if (allocation == IntPtr.Zero) throw new OutOfMemoryException();
        bool transferred = false;
        try {
            var pointer = GlobalLock(allocation);
            if (pointer == IntPtr.Zero) throw new InvalidOperationException("GlobalLock failed");
            try {
                Marshal.WriteInt32(pointer, 0, 20);
                Marshal.WriteInt32(pointer, 16, 1);
                Marshal.Copy(bytes, 0, IntPtr.Add(pointer, 20), bytes.Length);
            } finally { GlobalUnlock(allocation); }
            transferred = PostMessageW(window, 0x0233, allocation, IntPtr.Zero);
            if (!transferred) throw new InvalidOperationException("WM_DROPFILES failed");
        } finally { if (!transferred) GlobalFree(allocation); }
    }
}
'@

function Wait-Condition {
    param([scriptblock]$Condition, [string]$Description)
    $wait = [Diagnostics.Stopwatch]::StartNew()
    do {
        if (& $Condition) { return }
        Start-Sleep -Milliseconds 50
    } while ($wait.ElapsedMilliseconds -lt 15000)
    throw "Timed out: $Description"
}

function Save-Geometry {
    param([IntPtr]$Window, [string]$Stage)
    # Require a stable native layout, not a caption that can precede layout commit.
    $wait = [Diagnostics.Stopwatch]::StartNew()
    $stable = [Diagnostics.Stopwatch]::StartNew()
    $previous = ''
    do {
        $children = [PanelEditorOrderNative]::Children($Window)
        $signature = ConvertTo-Json -InputObject $children -Depth 4 -Compress
        if ($signature -ne $previous) { $previous = $signature; $stable.Restart() }
        if ($stable.ElapsedMilliseconds -ge 400) {
            $signature | Set-Content -LiteralPath (Join-Path $OutputDirectory "$Stage.json") -Encoding utf8
            return ,$children
        }
        Start-Sleep -Milliseconds 50
    } while ($wait.ElapsedMilliseconds -lt 15000)
    throw "Layout did not settle: $Stage"
}

function Find-Child {
    param($Children, [string]$Class)
    $matches = @($Children | Where-Object { $_.Class -eq $Class })
    if ($matches.Count -ne 1) { throw "Expected one $Class, found $($matches.Count)" }
    return $matches[0]
}

function Assert-NoOverlap {
    param($Children, [string]$Stage)
    $tabs = Find-Child $Children 'CTabWnd'
    $panel = Find-Child $Children 'SakuraBottomPanel'
    if (-not $panel.Visible) { throw "Panel is not visible: $Stage" }
    if ($tabs.Visible -and $tabs.Bounds.Bottom -gt $panel.Bounds.Top) {
        throw "Document chrome overlaps Panel by $($tabs.Bounds.Bottom - $panel.Bounds.Top) pixels: $Stage"
    }
    $terminal = Find-Child $Children 'SakuraNativeTerminalWindow'
    if (-not $terminal.Visible -or $terminal.Bounds.Top -le $panel.Bounds.Top) {
        throw "Terminal viewport/header is missing: $Stage"
    }
}

function Toggle-Maximized {
    param([IntPtr]$Window)
    $buttons = @([PanelEditorOrderNative]::Children($Window) | Where-Object {
        $_.Class -eq 'Button' -and $_.Id -eq 108 -and $_.Visible
    })
    if ($buttons.Count -ne 1) { throw "Expected one Panel maximize/restore button, found $($buttons.Count)" }
    [PanelEditorOrderNative]::Command([PanelEditorOrderNative]::GetParent([IntPtr]$buttons[0].Handle), 108)
}

function Assert-EditorHidden {
    param($Children, [string]$Stage)
    foreach ($class in @('CTabWnd', 'SplitterWndClass', 'SakuraWorkbenchEmptyEditorSurface')) {
        if ((Find-Child $Children $class).Visible) { throw "Editor Part child $class stayed visible: $Stage" }
    }
    Assert-NoOverlap $Children $Stage
}

function Assert-EditorRestored {
    param($Children, [int]$PanelHeight, [string]$Stage)
    $tabs = Find-Child $Children 'CTabWnd'
    $editor = Find-Child $Children 'SplitterWndClass'
    $panel = Find-Child $Children 'SakuraBottomPanel'
    if (-not $tabs.Visible -or -not $editor.Visible -or $editor.Bounds.Bottom -le $editor.Bounds.Top) {
        throw "Opening/restoring did not reveal the Editor Part: $Stage"
    }
    if ($panel.Bounds.Bottom - $panel.Bounds.Top -ne $PanelHeight) {
        throw "Panel did not restore its original height: $Stage"
    }
    Assert-NoOverlap $Children $Stage
}

function Remove-OwnedProfile {
    param([string]$ProfileName)
    $root = [IO.Path]::GetFullPath((Join-Path $env:APPDATA 'sakura'))
    $path = [IO.Path]::GetFullPath((Join-Path $root $ProfileName))
    if (-not $ProfileName.StartsWith('codex-panel-order-') -or
        -not $path.StartsWith($root + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Profile cleanup escaped its owned root: $path"
    }
    if ([IO.Directory]::Exists($path)) { [IO.Directory]::Delete($path, $true) }
    if ([IO.Directory]::Exists($path)) { throw "Profile cleanup failed: $path" }
}

$completed = 0
try {
    foreach ($order in @('terminal-first', 'file-first')) {
        foreach ($trial in 1..$Trials) {
            $profile = "codex-panel-order-$PID-$order-$trial"
            $process = $null
            try {
                Remove-OwnedProfile $profile
                $start = [Diagnostics.ProcessStartInfo]::new($Executable)
                $start.UseShellExecute = $false
                $start.ArgumentList.Add("-PROF=$profile")
                $start.ArgumentList.Add("-FOLDER=$repository")
                $process = [Diagnostics.Process]::Start($start)
                Wait-Condition {
                    $process.Refresh()
                    if ($process.HasExited) { throw "Editor exited during startup: $($process.ExitCode)" }
                    $process.MainWindowHandle -ne [IntPtr]::Zero
                } 'editor window'
                $window = $process.MainWindowHandle
                $null = Save-Geometry $window "$order-$trial-empty"
                if ($order -eq 'file-first') {
                    [PanelEditorOrderNative]::OpenFile($window, $document)
                    Wait-Condition { [PanelEditorOrderNative]::Title($window) -like '*CLAUDE.md*' } 'first file'
                    $null = Save-Geometry $window "$order-$trial-file"
                }
                [PanelEditorOrderNative]::Command($window, 30993) # F_TOGGLE_BOTTOM_PANEL
                Wait-Condition { @([PanelEditorOrderNative]::Children($window) | Where-Object {
                    $_.Class -eq 'SakuraNativeTerminalWindow' -and $_.Visible
                }).Count -eq 1 } 'terminal creation'
                $before = Save-Geometry $window "$order-$trial-before"
                $panel = Find-Child $before 'SakuraBottomPanel'
                $originalHeight = $panel.Bounds.Bottom - $panel.Bounds.Top
                Toggle-Maximized $window
                $maximized = Save-Geometry $window "$order-$trial-maximized"
                if ((Find-Child $maximized 'SakuraBottomPanel').Bounds.Top -ge
                    (Find-Child $before 'SakuraBottomPanel').Bounds.Top) { throw 'Panel did not maximize' }
                Assert-EditorHidden $maximized "$order-$trial-maximized"
                if ($order -eq 'terminal-first') {
                    [PanelEditorOrderNative]::OpenFile($window, $document)
                    Wait-Condition { [PanelEditorOrderNative]::Title($window) -like '*CLAUDE.md*' } 'first file'
                }
                $after = Save-Geometry $window "$order-$trial-after"
                if ($order -eq 'terminal-first') {
                    Assert-EditorRestored $after $originalHeight "$order-$trial-open-reveals-editor"
                    Toggle-Maximized $window
                    $maximized = Save-Geometry $window "$order-$trial-maximized-again"
                    Assert-EditorHidden $maximized "$order-$trial-maximized-again"
                } else {
                    Assert-EditorHidden $after "$order-$trial-remains-maximized"
                }
                [PanelEditorOrderNative]::RefreshEditorSnapshot($window)
                $refreshed = Save-Geometry $window "$order-$trial-snapshot-refresh"
                Assert-EditorHidden $refreshed "$order-$trial-snapshot-does-not-reveal"
                Toggle-Maximized $window
                $restored = Save-Geometry $window "$order-$trial-restored"
                Assert-EditorRestored $restored $originalHeight "$order-$trial-restored"
                ++$completed
                Write-Output "PASS $order trial $trial"
            } finally {
                if ($null -ne $process) {
                    if (-not $process.HasExited) {
                        $null = $process.CloseMainWindow()
                        if (-not $process.WaitForExit(4000)) { $process.Kill($true); $process.WaitForExit() }
                    }
                    $process.Dispose()
                }
                $owned = @(Get-CimInstance Win32_Process -Filter "Name = 'sakura.exe'" | Where-Object {
                    $_.CommandLine -like "*-PROF=$profile*"
                })
                foreach ($child in $owned) { Stop-Process -Id $child.ProcessId -Force -ErrorAction SilentlyContinue }
                Wait-Condition {
                    @(Get-CimInstance Win32_Process -Filter "Name = 'sakura.exe'" | Where-Object {
                        $_.CommandLine -like "*-PROF=$profile*"
                    }).Count -eq 0
                } 'test-owned editor/control process exit'
                Remove-OwnedProfile $profile
            }
        }
    }
} finally {
    Write-Output "Completed $completed/$($Trials * 2) trials; elapsed $([Math]::Round($timer.Elapsed.TotalSeconds, 2)) seconds."
}
