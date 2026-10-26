// Graphics.cs — the WinForms drawing stack over the homemade GDI HDC
// (win/gdi32.c over the compositor's canvas). Mirrors System.Drawing.Graphics
// for managed callers (Microsoft's WinForms is a Windows-only binary, so losedoze
// ships its own in this namespace).
using System;
using System.Runtime.InteropServices;

namespace System.Drawing {

public class Font
{
    public Font() { Name = "Liberation Sans"; Size = 12f; Style = FontStyle.Regular; }
    public Font(string name, float size, FontStyle style = FontStyle.Regular)
    { Name = name; Size = size; Style = style; }
    public string Name { get; }
    public float Size { get; }
    public FontStyle Style { get; }
}

public enum FontStyle { Regular = 0, Bold = 1, Italic = 2, Underline = 4, Strikeout = 8 }

public abstract class Brush { public System.Drawing.Color Color { get; set; } = System.Drawing.Color.White; }
public sealed class SolidBrush : Brush { public SolidBrush(System.Drawing.Color c) { Color = c; } }

public class Pen
{
    public Pen(System.Drawing.Color c, int width = 1) { Color = c; Width = width; }
    public System.Drawing.Color Color { get; }
    public int Width { get; }
}

public sealed class Graphics
{
    private readonly IntPtr _dc;
    internal int Ox, Oy;   // content offset (parent origin for children)

    internal Graphics(IntPtr dc) { _dc = dc; }

    public IntPtr Hdc => _dc;

    public void Clear(System.Drawing.Color c) { FillRectangle(c, new Rectangle(0, 0, int.MaxValue, int.MaxValue)); }

    public void FillRectangle(System.Drawing.Color c, Rectangle r)
    { FillRectangle(new SolidBrush(c), r); }

    public void FillRectangle(Brush b, Rectangle r)
    {
        var nr = new System.Windows.Forms.Native.RECT
        {
            left = (int)(r.Left + Ox),
            top = (int)(r.Top + Oy),
            right = (int)(r.Right + Ox),
            bottom = (int)(r.Bottom + Oy),
        };
        IntPtr handle = System.Windows.Forms.Native.CreateSolidBrush(
            (uint)((b.Color.B << 16) | (b.Color.G << 8) | b.Color.R));
        System.Windows.Forms.Native.FillRect(_dc, ref nr, handle);
        System.Windows.Forms.Native.DeleteObject(handle);
    }

    public void DrawRectangle(Pen p, Rectangle r)
    {
        var size = new Rectangle((int)(r.Left + Ox), (int)(r.Top + Oy), (int)r.Width, (int)r.Height);
        IntPtr pen = System.Windows.Forms.Native.CreatePen(0, p.Width, (uint)((p.Color.B << 16) | (p.Color.G << 8) | p.Color.R));
        IntPtr old = System.Windows.Forms.Native.SelectObject(_dc, pen);
        System.Windows.Forms.Native.Rectangle(_dc, size.Left, size.Top, size.Left + size.Width, size.Top + size.Height);
        System.Windows.Forms.Native.DeleteObject(pen);
        System.Windows.Forms.Native.SelectObject(_dc, old);
    }

    public void DrawRectangle(Pen p, int l, int t, int r, int b)
    {
        var size = new Rectangle((int)(l + Ox), (int)(t + Oy), (int)(r - l), (int)(b - t));
        IntPtr pen = System.Windows.Forms.Native.CreatePen(0, p.Width, (uint)((p.Color.B << 16) | (p.Color.G << 8) | p.Color.R));
        IntPtr old = System.Windows.Forms.Native.SelectObject(_dc, pen);
        System.Windows.Forms.Native.Rectangle(_dc, size.Left, size.Top, size.Left + size.Width, size.Top + size.Height);
        System.Windows.Forms.Native.DeleteObject(pen);
        System.Windows.Forms.Native.SelectObject(_dc, old);
    }

    public void DrawLine(Pen p, int x1, int y1, int x2, int y2)
    {
        IntPtr pen = System.Windows.Forms.Native.CreatePen(0, p.Width, (uint)((p.Color.B << 16) | (p.Color.G << 8) | p.Color.R));
        IntPtr old = System.Windows.Forms.Native.SelectObject(_dc, pen);
        var pt = new System.Windows.Forms.Native.POINT();
        System.Windows.Forms.Native.MoveToEx(_dc, x1 + Ox, y1 + Oy, ref pt);
        System.Windows.Forms.Native.LineTo(_dc, x2 + Ox, y2 + Oy);
        System.Windows.Forms.Native.SelectObject(_dc, old);
        System.Windows.Forms.Native.DeleteObject(pen);
    }

    public void DrawString(string s, Font f, System.Drawing.Color c, float x, float y)
    {
        var col = (int)((c.B << 16) | (c.G << 8) | c.R);
        System.Windows.Forms.Native.SetFontSize(_dc, f?.Size ?? 12f);
        System.Windows.Forms.Native.SetTextColor(_dc, (uint)col);
        System.Windows.Forms.Native.SetBkMode(_dc, System.Windows.Forms.Native.TRANSPARENT);
        System.Windows.Forms.Native.TextOutW(_dc, (int)(x + Ox), (int)(y + Oy), s, -1);
    }

    public SizeF MeasureString(string s, Font f) => MeasureString(s, f, new System.Drawing.Size(100000, 100000));

    public SizeF MeasureString(string s, Font f, Size proposed)
    {
        System.Windows.Forms.Native.SetFontSize(_dc, f?.Size ?? 12f);
        var size = new System.Windows.Forms.Native.SIZE();
        int ok = System.Windows.Forms.Native.GetTextExtentPoint32W(_dc, s, -1, ref size);
        if (ok == 0) return new SizeF(8f * (s?.Length ?? 0), f?.Size ?? 12f);
        return new SizeF(size.cx, size.cy);
    }
}
}
