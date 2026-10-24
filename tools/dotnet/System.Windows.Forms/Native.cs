// Native.cs — P/Invoke surface of libwin32.so into the homebrew Win32 layer.
// The types mirror win/w32.h / win/user32.c exactly (ms_abi on x64, stdcall on i386).
using System;
using System.Runtime.InteropServices;

namespace System.Windows.Forms {

internal static class Native
{
    public const string Lib = "win32";

    /* messages (win/w32.h) */
    public const uint WM_DESTROY = 0x0002, WM_MOVE = 0x0003, WM_SIZE = 0x0005,
        WM_PAINT = 0x000F, WM_CLOSE = 0x0010,
        WM_QUIT = 0x0012, WM_CHAR = 0x0102, WM_KEYDOWN = 0x0100, WM_KEYUP = 0x0101,
        WM_MOUSEMOVE = 0x0200, WM_LBUTTONDOWN = 0x0201, WM_LBUTTONUP = 0x0202,
        WM_RBUTTONDOWN = 0x0204, WM_RBUTTONUP = 0x0205;

    /* window styles */
    public const uint WS_OVERLAPPEDWINDOW = 0x00CF0000, WS_VISIBLE = 0x10000000,
        WS_OVERLAPPED = 0x00000000, WS_CAPTION = 0x00C00000, WS_BORDER = 0x00800000,
        WS_SYSMENU = 0x00080000, WS_THICKFRAME = 0x00040000, WS_MINIMIZEBOX = 0x00020000,
        WS_MAXIMIZEBOX = 0x00010000;

    /* styles */
    public const int TRANSPARENT = 1, OPAQUE = 2;

    /* virtual keys (subset) */
    public const int VK_BACK = 0x08, VK_TAB = 0x09, VK_RETURN = 0x0D, VK_ESCAPE = 0x1B,
        VK_PRIOR = 0x21, VK_NEXT = 0x22, VK_END = 0x23, VK_HOME = 0x24,
        VK_LEFT = 0x25, VK_UP = 0x26, VK_RIGHT = 0x27, VK_DOWN = 0x28, VK_INSERT = 0x2D,
        VK_DELETE = 0x2E;

    public static uint Rgb(int r, int g, int b) => (uint)((r << 16) | (g << 8) | b);

    /* messagebox */
    public const int MB_OK = 0, IDOK = 1, IDCANCEL = 2;

    public static uint ToColorRef(System.Drawing.Color c)
        => (uint)((c.B << 16) | (c.G << 8) | c.R);

    [StructLayout(LayoutKind.Sequential)]
    public struct POINT { public int x, y; }

    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int left, top, right, bottom; }

    [StructLayout(LayoutKind.Sequential)]
    public struct SIZE { public int cx, cy; }

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

    [DllImport(Lib, CallingConvention = CallingConvention.Winapi)]
    public static extern int los_init();

    [DllImport(Lib, CharSet = CharSet.Unicode)]
    public static extern ushort RegisterClassExW(ref WNDCLASSW wc);

    [DllImport(Lib, CharSet = CharSet.Unicode)]
    public static extern IntPtr CreateWindowExW(uint exStyle,
        [MarshalAs(UnmanagedType.LPWStr)] string cls,
        [MarshalAs(UnmanagedType.LPWStr)] string title,
        uint style, int x, int y, int w, int h,
        IntPtr parent, IntPtr menu, IntPtr inst, IntPtr param);

    [DllImport(Lib, CallingConvention = CallingConvention.Winapi)]
    public static extern int GetMessageW(ref MSG m, IntPtr hwnd, uint min, uint max);
    [DllImport(Lib, CallingConvention = CallingConvention.Winapi)]
    public static extern int TranslateMessage(ref MSG m);
    [DllImport(Lib, CallingConvention = CallingConvention.Winapi)]
    public static extern IntPtr DispatchMessageW(ref MSG m);
    [DllImport(Lib, CallingConvention = CallingConvention.Winapi)]
    public static extern IntPtr DefWindowProcW(IntPtr h, uint msg, ulong wp, long lp);
    [DllImport(Lib, CallingConvention = CallingConvention.Winapi)]
    public static extern int PeekMessageW(ref MSG m, IntPtr hwnd, uint min, uint max, uint flags);
    [DllImport(Lib, CallingConvention = CallingConvention.Winapi)]
    public static extern int PostMessageW(IntPtr h, uint msg, ulong wp, long lp);
    [DllImport(Lib, CallingConvention = CallingConvention.Winapi)]
    public static extern int ShowWindow(IntPtr h, int cmd);
    [DllImport(Lib, CallingConvention = CallingConvention.Winapi)]
    public static extern int DestroyWindow(IntPtr h);
    [DllImport(Lib, CallingConvention = CallingConvention.Winapi)]
    public static extern void PostQuitMessage(int code);

    [DllImport(Lib, CallingConvention = CallingConvention.Winapi)]
    public static extern IntPtr BeginPaint(IntPtr h, ref PAINTSTRUCT ps);
    [DllImport(Lib, CallingConvention = CallingConvention.Winapi)]
    public static extern int EndPaint(IntPtr h, ref PAINTSTRUCT ps);
    [DllImport(Lib, CallingConvention = CallingConvention.Winapi)]
    public static extern int InvalidateRect(IntPtr h, ref RECT r, int erase);
    [DllImport(Lib, CallingConvention = CallingConvention.Winapi)]
    public static extern int ValidateRect(IntPtr h, ref RECT r);
    [DllImport(Lib, CallingConvention = CallingConvention.Winapi)]
    public static extern IntPtr CreateSolidBrush(uint col);
    [DllImport(Lib, CallingConvention = CallingConvention.Winapi)]
    public static extern IntPtr CreatePen(int style, int width, uint col);
    [DllImport(Lib, CallingConvention = CallingConvention.Winapi)]
    public static extern IntPtr SelectObject(IntPtr dc, IntPtr obj);
    [DllImport(Lib, CallingConvention = CallingConvention.Winapi)]
    public static extern int DeleteObject(IntPtr obj);
    [DllImport(Lib, CallingConvention = CallingConvention.Winapi)]
    public static extern int FillRect(IntPtr dc, ref RECT r, IntPtr brush);
    [DllImport(Lib, CallingConvention = CallingConvention.Winapi)]
    public static extern int Rectangle(IntPtr dc, int l, int t, int r, int b);
    [DllImport(Lib, CallingConvention = CallingConvention.Winapi)]
    public static extern int MoveToEx(IntPtr dc, int x, int y, ref POINT old);
    [DllImport(Lib, CallingConvention = CallingConvention.Winapi)]
    public static extern int LineTo(IntPtr dc, int x, int y);
    [DllImport(Lib, CallingConvention = CallingConvention.Winapi)]
    public static extern uint SetTextColor(IntPtr dc, uint col);
    [DllImport(Lib, CallingConvention = CallingConvention.Winapi)]
    public static extern int SetBkMode(IntPtr dc, int mode);
    [DllImport(Lib, CallingConvention = CallingConvention.Winapi)]
    public static extern int TextOutW(IntPtr dc, int x, int y,
        [MarshalAs(UnmanagedType.LPWStr)] string s, int n);
    [DllImport(Lib, CallingConvention = CallingConvention.Winapi)]
    public static extern int GetTextExtentPoint32W(IntPtr dc, [MarshalAs(UnmanagedType.LPWStr)] string s, int n, ref SIZE size);
    [DllImport(Lib, CallingConvention = CallingConvention.Winapi)]
    public static extern int SetFontSize(IntPtr dc, float px);
    [DllImport(Lib, CallingConvention = CallingConvention.Winapi)]
    public static extern int SelectClipRect(IntPtr dc, ref RECT r);
    [DllImport(Lib, CallingConvention = CallingConvention.Winapi)]
    public static extern int IntersectClipRect(IntPtr dc, int l, int t, int r, int b);
    [DllImport(Lib, CallingConvention = CallingConvention.Winapi)]
    public static extern int GetClipBox(IntPtr dc, ref RECT r);
    [DllImport(Lib, CallingConvention = CallingConvention.Winapi)]
    public static extern int SaveDC(IntPtr dc);
    [DllImport(Lib, CallingConvention = CallingConvention.Winapi)]
    public static extern int RestoreDC(IntPtr dc, int saved);
    [DllImport(Lib, CallingConvention = CallingConvention.Winapi)]
    public static extern int SwapBuffers(IntPtr dc);
    [DllImport(Lib, CallingConvention = CallingConvention.Winapi)]
    public static extern IntPtr wglCreateContext(IntPtr dc);
    [DllImport(Lib, CallingConvention = CallingConvention.Winapi)]
    public static extern int wglMakeCurrent(IntPtr dc, IntPtr rc);
    [DllImport(Lib, CallingConvention = CallingConvention.Winapi)]
    public static extern int wglDeleteContext(IntPtr rc);
    [DllImport(Lib, CharSet = CharSet.Ansi)]
    public static extern IntPtr wglGetProcAddress(string name);
    [DllImport(Lib, CallingConvention = CallingConvention.Winapi)]
    public static extern IntPtr glGetString(uint name);
    [DllImport(Lib, CharSet = CharSet.Unicode)]
    public static extern IntPtr GetDC(IntPtr h);
    [DllImport(Lib, CallingConvention = CallingConvention.Winapi)]
    public static extern int ReleaseDC(IntPtr h, IntPtr dc);
    [DllImport(Lib, CharSet = CharSet.Unicode)]
    public static extern uint SetWindowTextW(IntPtr h, [MarshalAs(UnmanagedType.LPWStr)] string t);
    [DllImport(Lib, CharSet = CharSet.Unicode)]
    public static extern int GetWindowTextW(IntPtr h, System.Text.StringBuilder o, int cap);

    [DllImport(Lib, CallingConvention = CallingConvention.Winapi)]
    public static extern int MessageBoxW(IntPtr h, [MarshalAs(UnmanagedType.LPWStr)] string text,
        [MarshalAs(UnmanagedType.LPWStr)] string caption, uint flags);
}
}
