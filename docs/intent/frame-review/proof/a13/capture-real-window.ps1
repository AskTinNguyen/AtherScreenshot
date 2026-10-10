# A13: opens the build's video editor on its own (--edit-video: no tray app, no single-instance hand-off, a throwaway
# support folder), drives it only by messages posted to its window, and captures the real window (title bar included)
# with PrintWindow. Never touches a running copy of Ather Screenshot or %APPDATA%\AtherScreenshot.
#   powershell -File capture-real-window.ps1 [-work <folder>] [-exe <path to build\AtherScreenshot.exe>]
param([string]$work = "$env:TEMP\ather-frame-review-proof", [string]$exe = "$PSScriptRoot\..\..\..\..\..\build\AtherScreenshot.exe")
$ErrorActionPreference = "Stop"
$s = $work
New-Item -ItemType Directory -Force "$s\snaps", "$s\snap-support" | Out-Null
# The 60 fps clip (every frame shows its number) and the review pictures come from --video-snapshots.
$env:ATHER_SUPPORT_DIR = "$s\snap-support"
Start-Process -FilePath $exe -ArgumentList "--video-snapshots", "`"$s\snaps`"" -Wait
$dir = "$s\realwin"
Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $dir, "$dir\support" | Out-Null
Copy-Item "$s\snaps\snapshot-60fps.mp4" "$dir\gameplay 60fps.mp4"
# Notes kept next to the video, as the editor writes them (time = frame / 60).
$notes = @(
  @{f=95;  k="note";     r=$false; a="Tin Nguyen"; t="Intro starts a beat late"},
  @{f=260; k="issue";    r=$false; a="Tin Nguyen"; t="Health bar flickers for one frame when the shield breaks"; e=290},
  @{f=410; k="question"; r=$false; a="Mai";        t="Is this hit-stop intended?"},
  @{f=540; k="good";     r=$true;  a="Mai";        t="Dash trail reads well now"},
  @{f=742; k="question"; r=$false; a="Mai";        t="Recoil starts here?"},
  @{f=757; k="issue";    r=$false; a="Tin Nguyen"; t="Muzzle flash missing on this frame"; px=0.59; py=0.38}
)
$lines = @()
$id = 1
foreach ($n in $notes) {
  $o = '{"id":' + $id + ',"frame":' + $n.f + ',"time":' + ($n.f / 60.0).ToString([Globalization.CultureInfo]::InvariantCulture) + ',"kind":"' + $n.k + '","resolved":' + $n.r.ToString().ToLower() + ',"author":"' + $n.a + '","text":"' + $n.t + '"'
  if ($n.e) { $o += ',"endFrame":' + $n.e + ',"endTime":' + ($n.e / 60.0).ToString([Globalization.CultureInfo]::InvariantCulture) }
  if ($n.px) { $o += ',"pin":{"x":' + $n.px.ToString([Globalization.CultureInfo]::InvariantCulture) + ',"y":' + $n.py.ToString([Globalization.CultureInfo]::InvariantCulture) + '}' }
  $o += ',"created":"2026-10-10T09:00:00Z"}'
  $lines += $o
  $id++
}
$json = '{"app":"Ather Screenshot","format":1,"video":"gameplay 60fps.mp4","fps":60,"notes":[' + "`n  " + ($lines -join ",`n  ") + "`n]}`n"
[IO.File]::WriteAllText("$dir\gameplay 60fps.mp4.notes.json", $json, (New-Object Text.UTF8Encoding $false))

Add-Type @"
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Text;
public static class W {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint f);
  [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr c);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  public static IntPtr Find(uint pid, string cls) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); var sb = new StringBuilder(256); GetClassName(h, sb, 256);
      if (p == pid && sb.ToString() == cls && IsWindowVisible(h)) { found = h; return false; } return true; }, IntPtr.Zero);
    return found;
  }
  public static void Capture(IntPtr h, string path) {
    RECT r; GetWindowRect(h, out r);
    using (var b = new Bitmap(r.R - r.L, r.B - r.T, PixelFormat.Format32bppArgb)) {
      using (var g = Graphics.FromImage(b)) { var dc = g.GetHdc(); PrintWindow(h, dc, 2); g.ReleaseHdc(dc); }
      b.Save(path, ImageFormat.Png);
    }
  }
  static IntPtr XY(int x, int y) { return (IntPtr)((y << 16) | (x & 0xFFFF)); }
  public static void Click(IntPtr h, int x, int y) {
    PostMessage(h, 0x0200, IntPtr.Zero, XY(x, y));      // WM_MOUSEMOVE
    PostMessage(h, 0x0201, (IntPtr)1, XY(x, y));         // WM_LBUTTONDOWN
    PostMessage(h, 0x0202, IntPtr.Zero, XY(x, y));       // WM_LBUTTONUP
  }
  public static void Wheel(IntPtr h, int x, int y, int delta, int keys) {  // x, y in client pixels
    POINT p; p.X = x; p.Y = y; ClientToScreen(h, ref p);
    PostMessage(h, 0x020A, (IntPtr)((delta << 16) | keys), XY(p.X, p.Y));
  }
}
"@ -ReferencedAssemblies System.Drawing

[void][W]::SetThreadDpiAwarenessContext([IntPtr](-4))  # physical pixels, like the editor (the host process ignores a process-wide change)
$env:ATHER_SUPPORT_DIR = "$dir\support"
$p = Start-Process -FilePath $exe -ArgumentList "--edit-video", "`"$dir\gameplay 60fps.mp4`"" -PassThru
$h = [IntPtr]::Zero
for ($i = 0; $i -lt 100 -and $h -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 100; $h = [W]::Find([uint32]$p.Id, "AtherScreenshotVideoEditor") }
if ($h -eq [IntPtr]::Zero) { "no window"; Stop-Process -Id $p.Id -Force; exit 1 }
$dpi = [W]::GetDpiForWindow($h); $k = $dpi / 96.0
# left where and how big the editor opens itself (moving it to another monitor changes its DPI mid-run)
Start-Sleep -Seconds 4   # frame times, thumbnails
$c = New-Object W+RECT; [void][W]::GetClientRect($h, [ref]$c)
function S([double]$v) { [int][Math]::Round($v * $k) }
$cw = $c.R; $ch = $c.B
"dpi $dpi client $cw x $ch"
[W]::Capture($h, "$dir\a13-1-editor-open.png")
# The notes list: row 5 (frame 757, the pinned Issue).
$right = $cw - (S 14); $listTop = (S 50) + (S 44)
[W]::Click($h, $right - (S 150), $listTop + (S 52) * 5 + (S 26))
Start-Sleep -Seconds 2
[W]::Capture($h, "$dir\a13-2-note-selected.png")
# Ctrl+wheel over the timeline: zoomed all the way in around the playhead.
$tlTop = $ch - (S 14) - ((S 90) + (S 18) * 3)
for ($i = 0; $i -lt 25; $i++) { [W]::Wheel($h, [int]($cw / 2), $tlTop + (S 26), 120, 0x0008) }
Start-Sleep -Seconds 1
[W]::Capture($h, "$dir\a13-3-timeline-zoomed.png")
# The wheel over the video: the magnifier at 4x around the pinned spot.
$readTop = $tlTop - (S 8) - (S 30) - (S 4) - (S 22)
$stageL = S 14; $stageT = S 50; $stageR = $cw - (S 14) - (S 290) - (S 8); $stageB = $readTop - (S 4)
$sw = $stageR - $stageL; $sh = $stageB - $stageT
$scale = [Math]::Min($sw / 1280.0, $sh / 720.0)
$vx = $stageL + ($sw - 1280 * $scale) / 2; $vy = $stageT + ($sh - 720 * $scale) / 2
[W]::Wheel($h, [int]($vx + 0.59 * 1280 * $scale), [int]($vy + 0.38 * 720 * $scale), 745, 0)
Start-Sleep -Seconds 1
[W]::Capture($h, "$dir\a13-4-magnifier-4x.png")
[void][W]::PostMessage($h, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)  # WM_CLOSE
if (-not $p.WaitForExit(5000)) { Stop-Process -Id $p.Id -Force }
"exit $($p.ExitCode)"
Get-ChildItem $dir -Filter *.png | Select-Object Name, Length | Format-Table -AutoSize | Out-String
