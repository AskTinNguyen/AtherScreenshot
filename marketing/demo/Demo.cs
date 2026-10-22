// MARKETING DEMO ONLY: real mouse and keyboard input (SendInput) and window lookups for demo.ps1, which drives the
// real Ather Screenshot app on GitHub's Windows runner while ffmpeg records the desktop.
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

public static class Demo {
    [StructLayout(LayoutKind.Sequential)] struct MOUSEINPUT { public int dx, dy; public uint mouseData, dwFlags, time; public IntPtr extra; }
    [StructLayout(LayoutKind.Sequential)] struct KEYBDINPUT { public ushort wVk, wScan; public uint dwFlags, time; public IntPtr extra; }
    [StructLayout(LayoutKind.Explicit)] struct UNION { [FieldOffset(0)] public MOUSEINPUT mi; [FieldOffset(0)] public KEYBDINPUT ki; }
    [StructLayout(LayoutKind.Sequential)] struct INPUT { public uint type; public UNION u; }
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct DEVMODE {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmDeviceName;
        public short dmSpecVersion, dmDriverVersion, dmSize, dmDriverExtra; public int dmFields;
        public int dmPositionX, dmPositionY, dmDisplayOrientation, dmDisplayFixedOutput;
        public short dmColor, dmDuplex, dmYResolution, dmTTOption, dmCollate;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmFormName;
        public short dmLogPixels; public int dmBitsPerPel, dmPelsWidth, dmPelsHeight, dmDisplayFlags, dmDisplayFrequency;
        public int dmICMMethod, dmICMIntent, dmMediaType, dmDitherType, dmReserved1, dmReserved2, dmPanningWidth, dmPanningHeight;
    }
    [DllImport("user32.dll", SetLastError = true)] static extern uint SendInput(uint n, INPUT[] inputs, int size);
    [DllImport("user32.dll")] static extern bool GetCursorPos(out POINT p);
    [DllImport("user32.dll")] static extern int GetSystemMetrics(int i);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string title);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h, int x, int y, int w, int hh, bool repaint);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern bool EnumDisplaySettings(string dev, int mode, ref DEVMODE dm);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int ChangeDisplaySettings(ref DEVMODE dm, int flags);
    [DllImport("user32.dll")] static extern bool SetProcessDPIAware();

    static Demo() { SetProcessDPIAware(); }

    public static string Resolution(int w, int h) {
        DEVMODE dm = new DEVMODE(); dm.dmSize = (short)Marshal.SizeOf(typeof(DEVMODE));
        EnumDisplaySettings(null, -1, ref dm);
        string before = dm.dmPelsWidth + "x" + dm.dmPelsHeight;
        dm.dmPelsWidth = w; dm.dmPelsHeight = h; dm.dmFields = 0x80000 | 0x100000;
        int r = ChangeDisplaySettings(ref dm, 0);
        return "was " + before + ", ChangeDisplaySettings=" + r + ", now " + GetSystemMetrics(0) + "x" + GetSystemMetrics(1);
    }

    static void Send(INPUT i) { SendInput(1, new INPUT[] { i }, Marshal.SizeOf(typeof(INPUT))); }
    static void MouseAbs(int x, int y, uint flags) {
        int vx = GetSystemMetrics(76), vy = GetSystemMetrics(77), vw = GetSystemMetrics(78), vh = GetSystemMetrics(79);
        INPUT i = new INPUT(); i.type = 0;
        i.u.mi.dx = (int)Math.Round((x - vx) * 65535.0 / (vw - 1));
        i.u.mi.dy = (int)Math.Round((y - vy) * 65535.0 / (vh - 1));
        i.u.mi.dwFlags = 0x0001 | 0x8000 | 0x4000 | flags;  // MOVE | ABSOLUTE | VIRTUALDESK
        Send(i);
    }
    public static POINT Cursor() { POINT p; GetCursorPos(out p); return p; }
    // Glides to (x, y) over ms, eased, like a hand.
    public static void Move(int x, int y, int ms) {
        POINT p = Cursor();
        int steps = Math.Max(1, ms / 12);
        for (int k = 1; k <= steps; k++) {
            double t = (double)k / steps, e = t < 0.5 ? 2 * t * t : 1 - Math.Pow(-2 * t + 2, 2) / 2;
            MouseAbs((int)Math.Round(p.X + (x - p.X) * e), (int)Math.Round(p.Y + (y - p.Y) * e), 0);
            Thread.Sleep(12);
        }
    }
    public static void Down() { POINT p = Cursor(); MouseAbs(p.X, p.Y, 0x0002); }
    public static void Up() { POINT p = Cursor(); MouseAbs(p.X, p.Y, 0x0004); }
    public static void Click(int x, int y, int ms) { Move(x, y, ms); Thread.Sleep(120); Down(); Thread.Sleep(70); Up(); }
    public static void Drag(int x1, int y1, int x2, int y2, int ms) {
        Move(x1, y1, 450); Thread.Sleep(150); Down(); Thread.Sleep(120); Move(x2, y2, ms); Thread.Sleep(150); Up();
    }
    static bool Extended(ushort vk) { return vk == 0x2C || (vk >= 0x21 && vk <= 0x2E) || vk == 0x5B; }
    public static void KeyDown(ushort vk) { INPUT i = new INPUT(); i.type = 1; i.u.ki.wVk = vk; i.u.ki.dwFlags = Extended(vk) ? 1u : 0u; Send(i); }
    public static void KeyUp(ushort vk) { INPUT i = new INPUT(); i.type = 1; i.u.ki.wVk = vk; i.u.ki.dwFlags = 2u | (Extended(vk) ? 1u : 0u); Send(i); }
    // e.g. Chord(0x11, 0x12, 'E') = Ctrl+Alt+E
    public static void Chord(params ushort[] vks) {
        foreach (ushort v in vks) { KeyDown(v); Thread.Sleep(40); }
        Thread.Sleep(60);
        for (int k = vks.Length - 1; k >= 0; k--) { KeyUp(vks[k]); Thread.Sleep(30); }
    }
    public static void Type(string s, int msPerChar) {
        foreach (char ch in s) {
            INPUT d = new INPUT(); d.type = 1; d.u.ki.wScan = ch; d.u.ki.dwFlags = 4; Send(d);
            INPUT u = new INPUT(); u.type = 1; u.u.ki.wScan = ch; u.u.ki.dwFlags = 4 | 2; Send(u);
            Thread.Sleep(msPerChar);
        }
    }
    // Client rect in screen coordinates: {left, top, width, height}.
    public static int[] Client(IntPtr h) {
        RECT r; GetClientRect(h, out r); POINT p = new POINT(); ClientToScreen(h, ref p);
        return new int[] { p.X, p.Y, r.Right - r.Left, r.Bottom - r.Top };
    }
    public static int[] Window(IntPtr h) { RECT r; GetWindowRect(h, out r); return new int[] { r.Left, r.Top, r.Right - r.Left, r.Bottom - r.Top }; }
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string cls, string title);
    static List<IntPtr> All() {
        List<IntPtr> all = new List<IntPtr>();
        for (IntPtr h = FindWindowEx(IntPtr.Zero, IntPtr.Zero, null, null); h != IntPtr.Zero && all.Count < 5000; h = FindWindowEx(IntPtr.Zero, h, null, null)) all.Add(h);
        return all;
    }
    static string Text(IntPtr h) { StringBuilder t = new StringBuilder(512); GetWindowText(h, t, 512); return t.ToString(); }
    static string Cls(IntPtr h) { StringBuilder c = new StringBuilder(256); GetClassName(h, c, 256); return c.ToString(); }
    // Visible top-level windows whose title contains `part` (and class, if given), front to back.
    public static IntPtr[] Find(string cls, string part) {
        List<IntPtr> found = new List<IntPtr>();
        foreach (IntPtr h in All()) {
            if (!IsWindowVisible(h)) continue;
            if ((cls == null || Cls(h) == cls) && (part == null || Text(h).IndexOf(part, StringComparison.OrdinalIgnoreCase) >= 0)) found.Add(h);
        }
        return found.ToArray();
    }
    public static string List() {
        StringBuilder sb = new StringBuilder();
        List<IntPtr> all = All();
        sb.AppendFormat("  {0} top-level windows\n", all.Count);
        foreach (IntPtr h in all) {
            if (!IsWindowVisible(h)) continue;
            RECT r; GetWindowRect(h, out r);
            sb.AppendFormat("  {0} [{1}] '{2}' {3},{4} {5}x{6}\n", h, Cls(h), Text(h), r.Left, r.Top, r.Right - r.Left, r.Bottom - r.Top);
        }
        return sb.ToString();
    }

    // ---- desktop recorder: BitBlt with CAPTUREBLT (includes layered windows such as the app's toasts, which ffmpeg's
    // gdigrab leaves out) plus the cursor, piped to ffmpeg as raw BGRA at a constant 30 fps (late frames repeat).
    [StructLayout(LayoutKind.Sequential)] struct BITMAPINFOHEADER { public int biSize, biWidth, biHeight; public short biPlanes, biBitCount; public int biCompression, biSizeImage, biXPelsPerMeter, biYPelsPerMeter, biClrUsed, biClrImportant; }
    [StructLayout(LayoutKind.Sequential)] struct CURSORINFO { public int cbSize, flags; public IntPtr hCursor; public POINT pt; }
    [StructLayout(LayoutKind.Sequential)] struct ICONINFO { public bool fIcon; public int xHotspot, yHotspot; public IntPtr hbmMask, hbmColor; }
    [DllImport("user32.dll")] static extern IntPtr GetDC(IntPtr h);
    [DllImport("user32.dll")] static extern int ReleaseDC(IntPtr h, IntPtr dc);
    [DllImport("gdi32.dll")] static extern IntPtr CreateCompatibleDC(IntPtr dc);
    [DllImport("gdi32.dll")] static extern IntPtr CreateDIBSection(IntPtr dc, ref BITMAPINFOHEADER bi, uint usage, out IntPtr bits, IntPtr sec, uint off);
    [DllImport("gdi32.dll")] static extern IntPtr SelectObject(IntPtr dc, IntPtr o);
    [DllImport("gdi32.dll")] static extern bool DeleteObject(IntPtr o);
    [DllImport("gdi32.dll")] static extern bool BitBlt(IntPtr d, int x, int y, int w, int h, IntPtr s, int sx, int sy, uint rop);
    [DllImport("user32.dll")] static extern bool GetCursorInfo(ref CURSORINFO ci);
    [DllImport("user32.dll")] static extern bool GetIconInfo(IntPtr h, out ICONINFO ii);
    [DllImport("user32.dll")] static extern bool DrawIconEx(IntPtr dc, int x, int y, IntPtr icon, int w, int h, int step, IntPtr brush, int flags);
    static volatile bool capturing;
    static Thread capThread;
    static System.Diagnostics.Process capProc;
    public static int CapturedFrames, RepeatedFrames;
    public static void StartCapture(string ffmpeg, string outFile) {
        int w = GetSystemMetrics(0), h = GetSystemMetrics(1);
        var psi = new System.Diagnostics.ProcessStartInfo(ffmpeg, "-y -loglevel warning -f rawvideo -pix_fmt bgra -s " + w + "x" + h +
            " -framerate 30 -i - -c:v libx264 -preset veryfast -crf 18 -pix_fmt yuv420p \"" + outFile + "\"");
        psi.UseShellExecute = false; psi.RedirectStandardInput = true;
        capProc = System.Diagnostics.Process.Start(psi);
        capturing = true;
        capThread = new Thread(() => {
            IntPtr screen = GetDC(IntPtr.Zero), mem = CreateCompatibleDC(screen);
            BITMAPINFOHEADER bi = new BITMAPINFOHEADER(); bi.biSize = Marshal.SizeOf(typeof(BITMAPINFOHEADER)); bi.biWidth = w; bi.biHeight = -h; bi.biPlanes = 1; bi.biBitCount = 32;
            IntPtr bits; IntPtr dib = CreateDIBSection(screen, ref bi, 0, out bits, IntPtr.Zero, 0);
            SelectObject(mem, dib);
            byte[] frame = new byte[w * h * 4];
            var stdin = capProc.StandardInput.BaseStream;
            var clock = System.Diagnostics.Stopwatch.StartNew();
            long written = 0;
            while (capturing) {
                BitBlt(mem, 0, 0, w, h, screen, 0, 0, 0x00CC0020 | 0x40000000);  // SRCCOPY | CAPTUREBLT
                CURSORINFO ci = new CURSORINFO(); ci.cbSize = Marshal.SizeOf(typeof(CURSORINFO));
                if (GetCursorInfo(ref ci) && (ci.flags & 1) != 0) {
                    ICONINFO ii; int hx = 0, hy = 0;
                    if (GetIconInfo(ci.hCursor, out ii)) { hx = ii.xHotspot; hy = ii.yHotspot; if (ii.hbmMask != IntPtr.Zero) DeleteObject(ii.hbmMask); if (ii.hbmColor != IntPtr.Zero) DeleteObject(ii.hbmColor); }
                    DrawIconEx(mem, ci.pt.X - hx, ci.pt.Y - hy, ci.hCursor, 0, 0, 0, IntPtr.Zero, 3);
                }
                Marshal.Copy(bits, frame, 0, frame.Length);
                CapturedFrames++;
                long due = (long)(clock.Elapsed.TotalSeconds * 30) + 1;
                bool first = true;
                while (written < due) {  // keep 30 fps: a slow grab repeats the frame
                    try { stdin.Write(frame, 0, frame.Length); } catch { capturing = false; break; }
                    written++;
                    if (!first) RepeatedFrames++;
                    first = false;
                }
                int ahead = (int)(written * 1000 / 30 - clock.ElapsedMilliseconds);
                if (ahead > 0) Thread.Sleep(ahead);
            }
            DeleteObject(dib); ReleaseDC(IntPtr.Zero, screen);
        });
        capThread.IsBackground = true;
        capThread.Start();
    }
    public static int StopCapture() {
        capturing = false;
        capThread.Join(10000);
        capProc.StandardInput.Close();
        capProc.WaitForExit(120000);
        return capProc.ExitCode;
    }
}
