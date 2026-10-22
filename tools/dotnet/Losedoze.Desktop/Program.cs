// Program.cs — Losedoze .NET desktop app.
//
// A managed app that draws a real desktop window through the Win32 layer
// (libwin32.so): RegisterClassEx/CreateWindowEx + GDI painting + the message
// loop. The Win32 API is our own implementation over the losedoze compositor
// (no Wine, no Microsoft Windows Desktop runtime); the managed side below is a
// small WinForms-shaped wrapper (Form/Button/Label) on top of it.
//
// Marker: LOSEDOZE .NET DESKTOP OK <runtime> — printed after the first WM_PAINT
// reached the compositor buffer.

using System;
using System.Runtime.InteropServices;

namespace Losedoze.Desktop;

internal static class Win32
{
    public const string Lib = "win32";

    public const uint WM_DESTROY = 0x0002, WM_PAINT = 0x000F, WM_CLOSE = 0x0010,
                      WM_QUIT = 0x0012, WM_KEYDOWN = 0x0100, WM_LBUTTONDOWN = 0x0201,
                      WM_LBUTTONUP = 0x0202, WM_MOUSEMOVE = 0x0200;
    public const uint WS_OVERLAPPEDWINDOW = 0x00CF0000, WS_VISIBLE = 0x10000000;
    public const int TRANSPARENT = 1, OPAQUE = 2;
    public const int VK_ESCAPE = 0x1B, VK_RETURN = 0x0D;

    public static uint Rgb(int r, int g, int b) => (uint)((r << 16) | (g << 8) | b);

    [StructLayout(LayoutKind.Sequential)]
    public struct POINT { public int x, y; }

    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int left, top, right, bottom; }

    [StructLayout(LayoutKind.Sequential)]
    public struct MSG
    {
        public IntPtr hwnd;
        public uint message;
        public ulong wParam;
        public long lParam;
        public uint time;
        public POINT pt;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct PAINTSTRUCT
    {
        public IntPtr hdc;
        public int fErase;
        public RECT rcPaint;
        public int fRestore, fIncUpdate;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 32)] public byte[] rgbReserved;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct WNDCLASSW
    {
        public uint style;
        public WndProc lpfnWndProc;
        public int cbClsExtra, cbWndExtra;
        public IntPtr hInstance, hIcon, hCursor, hbrBackground;
        [MarshalAs(UnmanagedType.LPWStr)] public string lpszMenuName;
        [MarshalAs(UnmanagedType.LPWStr)] public string lpszClassName;
    }

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate IntPtr WndProc(IntPtr hwnd, uint msg, ulong wParam, long lParam);

    [DllImport(Lib)] public static extern int los_init();

    [DllImport(Lib, CharSet = CharSet.Unicode)]
    public static extern ushort RegisterClassExW(ref WNDCLASSW wc);

    [DllImport(Lib, CharSet = CharSet.Unicode)]
    public static extern IntPtr CreateWindowExW(uint exStyle,
        [MarshalAs(UnmanagedType.LPWStr)] string cls,
        [MarshalAs(UnmanagedType.LPWStr)] string title,
        uint style, int x, int y, int w, int h,
        IntPtr parent, IntPtr menu, IntPtr inst, IntPtr param);

    [DllImport(Lib)] public static extern int GetMessageW(ref MSG m, IntPtr hwnd, uint min, uint max);
    [DllImport(Lib)] public static extern int TranslateMessage(ref MSG m);
    [DllImport(Lib)] public static extern IntPtr DispatchMessageW(ref MSG m);
    [DllImport(Lib)] public static extern IntPtr DefWindowProcW(IntPtr h, uint msg, ulong wp, long lp);
    [DllImport(Lib)] public static extern int DestroyWindow(IntPtr h);
    [DllImport(Lib)] public static extern void PostQuitMessage(int code);

    [DllImport(Lib)] public static extern IntPtr BeginPaint(IntPtr h, ref PAINTSTRUCT ps);
    [DllImport(Lib)] public static extern int EndPaint(IntPtr h, ref PAINTSTRUCT ps);
    [DllImport(Lib)] public static extern int InvalidateRect(IntPtr h, IntPtr r, int erase);

    [DllImport(Lib)] public static extern IntPtr CreateSolidBrush(uint col);
    [DllImport(Lib)] public static extern IntPtr CreatePen(int style, int width, uint col);
    [DllImport(Lib)] public static extern IntPtr SelectObject(IntPtr dc, IntPtr obj);
    [DllImport(Lib)] public static extern int DeleteObject(IntPtr obj);
    [DllImport(Lib)] public static extern int FillRect(IntPtr dc, ref RECT r, IntPtr brush);
    [DllImport(Lib)] public static extern int Rectangle(IntPtr dc, int l, int t, int r, int b);
    [DllImport(Lib)] public static extern int MoveToEx(IntPtr dc, int x, int y, IntPtr old);
    [DllImport(Lib)] public static extern int LineTo(IntPtr dc, int x, int y);
    [DllImport(Lib)] public static extern uint SetTextColor(IntPtr dc, uint col);
    [DllImport(Lib)] public static extern int SetBkMode(IntPtr dc, int mode);
    [DllImport(Lib, CharSet = CharSet.Unicode)]
    public static extern int TextOutW(IntPtr dc, int x, int y,
        [MarshalAs(UnmanagedType.LPWStr)] string s, int n);
}

/// <summary>A window: native window class + message pump, painted by OnPaint.</summary>
internal class Form
{
    private readonly string _cls;
    private readonly Win32.WndProc _proc;   // rooted: native code calls back into it
    private IntPtr _hwnd;
    private int _w, _h;
    private readonly Button _btn;
    private int _clicks;

    public Form(string title, int w, int h)
    {
        Title = title;
        _w = w; _h = h;
        _cls = "LosedozeNetForm" + Guid.NewGuid().ToString("N")[..6];
        _proc = WndProc;
        _btn = new Button(24, 150, 200, 36, "Click me") { Click = OnClick };
    }

    public string Title { get; set; }
    public int Paints { get; private set; }

    public IntPtr Create()
    {
        var wc = new Win32.WNDCLASSW
        {
            lpfnWndProc = _proc,
            lpszClassName = _cls,
            hbrBackground = Win32.CreateSolidBrush(Win32.Rgb(0x1f, 0x1f, 0x1f)),
        };
        Win32.RegisterClassExW(ref wc);
        _hwnd = Win32.CreateWindowExW(0, _cls, Title,
            Win32.WS_OVERLAPPEDWINDOW | Win32.WS_VISIBLE, 0, 0, _w, _h,
            IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);
        return _hwnd;
    }

    private void OnClick() { _clicks++; Win32.InvalidateRect(_hwnd, IntPtr.Zero, 1); }

    private IntPtr WndProc(IntPtr hwnd, uint msg, ulong wp, long lp)
    {
        switch (msg)
        {
            case Win32.WM_PAINT:
                var ps = new Win32.PAINTSTRUCT { rgbReserved = new byte[32] };
                IntPtr dc = Win32.BeginPaint(hwnd, ref ps);
                OnPaint(dc);
                Win32.EndPaint(hwnd, ref ps);
                Paints++;
                break;
            case Win32.WM_LBUTTONDOWN:
                int x = (int)(lp & 0xffff), y = (int)((lp >> 16) & 0xffff);
                _btn.Hit(x, y);
                break;
            case Win32.WM_KEYDOWN:
                if (wp == Win32.VK_ESCAPE) Win32.PostQuitMessage(0);
                break;
            case Win32.WM_CLOSE:
                Win32.DestroyWindow(hwnd);
                Win32.PostQuitMessage(0);
                break;
        }
        return Win32.DefWindowProcW(hwnd, msg, wp, lp);
    }

    private void OnPaint(IntPtr dc)
    {
        uint bg = Win32.Rgb(0x1f, 0x1f, 0x1f), fg = Win32.Rgb(0xff, 0xff, 0xff);
        uint dim = Win32.Rgb(0x9a, 0x9a, 0x9a), accent = Win32.Rgb(0x2f, 0x6f, 0xed);
        var all = new Win32.RECT { left = 0, top = 0, right = _w, bottom = _h };
        IntPtr bgb = Win32.CreateSolidBrush(bg);
        Win32.FillRect(dc, ref all, bgb);
        Win32.DeleteObject(bgb);

        Win32.SetBkMode(dc, Win32.TRANSPARENT);
        Win32.SetTextColor(dc, fg);
        Win32.TextOutW(dc, 24, 24, "Losedoze .NET Desktop", -1);
        Win32.SetTextColor(dc, dim);
        Win32.TextOutW(dc, 24, 52, "managed C# over the Win32 layer (libwin32.so)", -1);
        Win32.TextOutW(dc, 24, 76, "window, GDI painting and the message loop are native", -1);

        // a GDI line + a filled rectangle: proves the DC is live
        IntPtr pen = Win32.CreatePen(0, 1, accent);
        IntPtr old = Win32.SelectObject(dc, pen);
        Win32.MoveToEx(dc, 24, 120, IntPtr.Zero);
        Win32.LineTo(dc, _w - 24, 120);
        Win32.SelectObject(dc, old);
        Win32.DeleteObject(pen);

        _btn.Paint(dc);
        Win32.SetTextColor(dc, dim);
        Win32.TextOutW(dc, 24, _h - 40, $"paints={Paints + 1} clicks={_clicks}", -1);
        Win32.TextOutW(dc, _w - 240, _h - 40, ".NET " + Environment.Version, -1);
    }

    public int Run()
    {
        var m = new Win32.MSG();
        while (Win32.GetMessageW(ref m, IntPtr.Zero, 0, 0) > 0)
        {
            if (m.message == Win32.WM_QUIT) break;
            Win32.TranslateMessage(ref m);
            Win32.DispatchMessageW(ref m);
        }
        return (int)m.wParam;
    }
}

/// <summary>A button: painted into the form's DC, hit-tested on mouse down.</summary>
internal class Button
{
    private readonly int _x, _y, _w, _h;
    private readonly string _text;
    public Action? Click;

    public Button(int x, int y, int w, int h, string text)
    { _x = x; _y = y; _w = w; _h = h; _text = text; }

    public void Paint(IntPtr dc)
    {
        IntPtr face = Win32.CreateSolidBrush(Win32.Rgb(0x3a, 0x3a, 0x3a));
        IntPtr old = Win32.SelectObject(dc, face);
        Win32.Rectangle(dc, _x, _y, _x + _w, _y + _h);
        Win32.SelectObject(dc, old);
        Win32.DeleteObject(face);
        Win32.SetTextColor(dc, Win32.Rgb(0xff, 0xff, 0xff));
        Win32.TextOutW(dc, _x + 12, _y + 11, _text, -1);
    }

    public void Hit(int x, int y)
    {
        if (x >= _x && x < _x + _w && y >= _y && y < _y + _h) Click?.Invoke();
    }
}

internal static class Program
{
    private static int Main()
    {
        bool compositor = Win32.los_init() != 0;
        Console.WriteLine("los: Win32 layer up (compositor=" + (compositor ? "yes" : "no") + ")");

        var form = new Form("Losedoze .NET Desktop", 560, 360);
        if (form.Create() == IntPtr.Zero)
        {
            Console.WriteLine("LOSEDOZE .NET DESKTOP FAIL (CreateWindowEx)");
            return 1;
        }

        // the first paint happens in the message loop: announce it from a watcher
        var watcher = new System.Threading.Thread(() =>
        {
            for (int i = 0; i < 200 && form.Paints == 0; i++)
                System.Threading.Thread.Sleep(50);
            Console.WriteLine(form.Paints > 0
                ? "LOSEDOZE .NET DESKTOP OK " + Environment.Version
                : "LOSEDOZE .NET DESKTOP FAIL (no paint)");
            Console.Out.Flush();
        });
        watcher.IsBackground = true;
        watcher.Start();

        return form.Run();
    }
}
