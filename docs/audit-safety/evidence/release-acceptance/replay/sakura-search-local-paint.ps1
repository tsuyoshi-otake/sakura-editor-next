param([string]$Configuration='Debug',[switch]$CompileOnly)
$ErrorActionPreference='Stop'
$taskRoot='C:/Users/developer/tmp/sakura-audit-safety'
$outRoot=Join-Path $taskRoot ".codex/goal-loop/audit-safety/search-paint-local-$Configuration"
[IO.Directory]::CreateDirectory($outRoot) | Out-Null
$clock=[Diagnostics.Stopwatch]::StartNew()
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
public static class SearchPaintCapture {
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left,Top,Right,Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct Point { public int X,Y; }
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr window,out Rect rect);
    [DllImport("user32.dll")] static extern IntPtr WindowFromPoint(Point point);
    [DllImport("user32.dll")] static extern IntPtr GetAncestor(IntPtr window,uint flags);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr window,out uint pid);
    [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr window,IntPtr dc,uint flags);
    [DllImport("user32.dll")] public static extern bool RedrawWindow(IntPtr window,IntPtr region,IntPtr update,uint flags);
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out Point point);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x,int y);
    public static double Capture(long handle,long parent,uint expectedPid,string prefix,string currentFile) {
        var window=new IntPtr(handle);
        uint pid;
        GetWindowThreadProcessId(window,out pid);
        if(pid!=expectedPid) throw new InvalidOperationException("Window ownership changed");
        Rect r;
        if(!GetWindowRect(window,out r)) throw new InvalidOperationException("Missing test window");
        for(int y=0;y<5;++y) for(int x=0;x<5;++x) {
            var p=new Point {X=r.Left+(r.Right-r.Left)*(2*x+1)/10,Y=r.Top+(r.Bottom-r.Top)*(2*y+1)/10};
            if(GetAncestor(WindowFromPoint(p),2).ToInt64()!=parent)
                throw new InvalidOperationException("Occluded test window");
        }
        int width=r.Right-r.Left,height=r.Bottom-r.Top;
        using(var screen=new Bitmap(width,height,PixelFormat.Format32bppRgb))
        using(var print=new Bitmap(width,height,PixelFormat.Format32bppRgb))
        using(var sg=Graphics.FromImage(screen))
        using(var pg=Graphics.FromImage(print)) {
            sg.CopyFromScreen(r.Left,r.Top,0,0,new Size(width,height),CopyPixelOperation.SourceCopy);
            var dc=pg.GetHdc();
            try { if(!PrintWindow(window,dc,2)) throw new InvalidOperationException("PrintWindow failed"); }
            finally { pg.ReleaseHdc(dc); }
            screen.Save(prefix+"-screen.png",ImageFormat.Png);
            print.Save(prefix+"-print.png",ImageFormat.Png);
            var bounds=new Rectangle(0,0,width,height);
            var a=screen.LockBits(bounds,ImageLockMode.ReadOnly,PixelFormat.Format32bppRgb);
            using(var current=new Bitmap(currentFile)) {
            current.Save(prefix+"-current.png",ImageFormat.Png);
            var b=current.LockBits(bounds,ImageLockMode.ReadOnly,PixelFormat.Format32bppRgb);
            int changed=0;
            using(var heat=new Bitmap(width,height,PixelFormat.Format32bppRgb)) {
                try {
                    byte[] ab=new byte[a.Stride*height],bb=new byte[b.Stride*height];
                    Marshal.Copy(a.Scan0,ab,0,ab.Length);Marshal.Copy(b.Scan0,bb,0,bb.Length);
                    for(int y=0;y<height;++y) for(int x=0;x<width;++x) {
                        int i=y*a.Stride+x*4,j=y*b.Stride+x*4;
                        if(ab[i]!=bb[j]||ab[i+1]!=bb[j+1]||ab[i+2]!=bb[j+2]) {
                            ++changed;heat.SetPixel(x,y,Color.Red);
                        }
                    }
                } finally {screen.UnlockBits(a);current.UnlockBits(b);}
                heat.Save(prefix+"-diff.png",ImageFormat.Png);
            }
            return 100.0*changed/(width*height);
            }
        }
    }
}
'@ -ReferencedAssemblies System.Drawing.Common,System.Drawing.Primitives,System.Private.Windows.GdiPlus,System.Private.Windows.Core
if($CompileOnly) { Write-Output 'Capture helper compiled'; exit 0 }
$eventBase='Local\SakuraAuditSearchPaint-'+[Guid]::NewGuid().ToString('N')
$ready=[Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::AutoReset,"$eventBase-ready")
$resume=[Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::AutoReset,"$eventBase-resume")
$env:SAKURA_AUDIT_PAINT_ROOT=$outRoot
$env:SAKURA_AUDIT_PAINT_EVENTS=$eventBase
$cursor=[SearchPaintCapture+Point]::new()
$haveCursor=[SearchPaintCapture]::GetCursorPos([ref]$cursor)
[SearchPaintCapture]::SetCursorPos(20,20) | Out-Null
$records=[Collections.Generic.List[object]]::new()
$process=$null
try {
    $process=Start-Process -FilePath "$taskRoot/x64/$Configuration/tests1.exe" -ArgumentList '--gtest_filter=SearchRequestSafetyTest.PrintClientRendersCurrentSearchSurfaceAndPreservesDcState:SearchRequestSafetyTest.DiagnosticVisibleInvalidationPaint',"--gtest_output=xml:$outRoot/result.xml" -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru -RedirectStandardOutput "$outRoot/runner.log" -RedirectStandardError "$outRoot/runner.err"
    for($trial=0;$trial -lt 30;$trial++) {
        if(!$ready.WaitOne(10000)) {throw "Capture step $trial was not reached"}
        $fields=[IO.File]::ReadAllText("$outRoot/step.txt").Trim().Split(' ')
        if([int]$fields[0] -ne $trial*2) {throw 'Unexpected step identity'}
        $window=[long]$fields[1]
        $parentWindow=[long]$fields[2]
        $prefix=Join-Path $outRoot ('trial-{0:d2}' -f $trial)
        $before=[SearchPaintCapture]::Capture($parentWindow,$parentWindow,$process.Id,$prefix,(Join-Path $outRoot "current-$($trial*2).bmp"))
        [SearchPaintCapture]::RedrawWindow([IntPtr]$parentWindow,[IntPtr]::Zero,[IntPtr]::Zero,0x185) | Out-Null
        Start-Sleep -Milliseconds 30
        $resume.Set() | Out-Null
        if(!$ready.WaitOne(10000)) {throw "Noise step $trial was not reached"}
        if([int]([IO.File]::ReadAllText("$outRoot/step.txt").Trim().Split(' ')[0]) -ne $trial*2+1) {throw 'Unexpected noise identity'}
        $noise=[SearchPaintCapture]::Capture($parentWindow,$parentWindow,$process.Id,"$prefix-noise",(Join-Path $outRoot "current-$($trial*2+1).bmp"))
        $records.Add([pscustomobject]@{trial=$trial;scenario=@('clear','debounce','root')[$trial%3];input_ms=[double]::Parse($fields[3],[Globalization.CultureInfo]::InvariantCulture);difference_percent=$before;noise_percent=$noise;pass=($noise -le 0.5 -and $before -le $noise+0.05)})
        $resume.Set() | Out-Null
    }
    if(!$process.WaitForExit(10000)) {throw 'Paint test did not exit'}
    if($process.ExitCode -ne 0) {throw "Paint test failed: $($process.ExitCode)"}
    if(@($records | Where-Object { !$_.pass }).Count) {throw 'Paint difference exceeds predeclared noise threshold'}
} finally {
    $resume.Set() | Out-Null
    if($process -and !$process.HasExited) {taskkill /PID $process.Id /T /F | Out-Null}
    if($haveCursor) {[SearchPaintCapture]::SetCursorPos($cursor.X,$cursor.Y) | Out-Null}
    $ready.Dispose();$resume.Dispose()
    Remove-Item Env:SAKURA_AUDIT_PAINT_ROOT,Env:SAKURA_AUDIT_PAINT_EVENTS
    $records | ConvertTo-Json -Depth 5 | Set-Content "$outRoot/measurements.json"
    & "$taskRoot/tools/Cleanup-TestProcessSurvivors.ps1" -RepositoryRoot $taskRoot -WaitSeconds 5
    Write-Output "ElapsedSeconds=$($clock.Elapsed.TotalSeconds) Trials=$($records.Count)"
}
