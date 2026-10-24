// AdvancedControls.cs — the second wave of WinForms-shaped controls for
// losedoze's managed desktop: DataGridView (owner-drawn grid), WebBrowser
// (text renderer for fetched/loaded documents) and GlContext (desktop GL for
// managed callers, over the shared library's EGL-backed opengl32 entry points).
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Runtime.InteropServices;

namespace System.Windows.Forms {

public class DataGridView : Control, System.ComponentModel.ISupportInitialize
{
    public DataGridView()
    {
        BackColor = SystemColors.Window;
        ForeColor = SystemColors.WindowText;
        Width = 320; Height = 160;
        RowHeight = 20; ColumnWidth = 90;
    }

    public List<string[]> Rows { get; } = new List<string[]>();
    public int ColumnCount { get; set; }
    public int RowHeight { get; set; }
    public int ColumnWidth { get; set; }
    public int SelectedIndex { get; set; } = -1;
    public int RowCount => Rows.Count;

    public void AddRow(params string[] cells)
    {
        Rows.Add(cells ?? System.Array.Empty<string>());
        if (cells != null && cells.Length > ColumnCount) ColumnCount = cells.Length;
        Invalidate();
    }

    public void ClearRows() { Rows.Clear(); SelectedIndex = -1; Invalidate(); }

    public void BeginInit() { }
    public void EndInit() { }

    internal override void PaintSelf(Graphics g)
    {
        g.FillRectangle(BackColor, new Rectangle(0, 0, Width, Height));
        int cols = ColumnCount < 1 ? 1 : ColumnCount;
        int totalW = cols * ColumnWidth;
        if (totalW > Width) totalW = Width;
        int rh = RowHeight < 1 ? 1 : RowHeight;
        for (int i = 0; i < Rows.Count; i++)
        {
            int y = i * rh;
            if (y >= Height) break;
            if (i == SelectedIndex) g.FillRectangle(SystemColors.Highlight, new Rectangle(0, y, totalW, rh));
            var cells = Rows[i];
            for (int c = 0; c < cols; c++)
            {
                int x = c * ColumnWidth;
                if (x >= Width) break;
                string txt = c < cells.Length ? cells[c] : "";
                g.DrawString(txt, Font, i == SelectedIndex ? SystemColors.HighlightText : ForeColor, x + 3, y + 3);
            }
        }
        g.DrawRectangle(new Pen(SystemColors.ControlDark), new Rectangle(0, 0, Width, Height));
        base.PaintSelf(g);
    }

    internal override void RaiseMouseDown(int x, int y, MouseButtons b)
    {
        int rh = RowHeight < 1 ? 1 : RowHeight;
        int i = y / rh;
        if (i >= 0 && i < Rows.Count) { SelectedIndex = i; Invalidate(); }
        base.RaiseMouseDown(x, y, b);
    }
}

public class WebBrowser : Control
{
    private string _text = "";
    private int _scroll;

    public WebBrowser()
    {
        BackColor = SystemColors.Window;
        ForeColor = SystemColors.WindowText;
        Width = 320; Height = 160;
    }

    public string DocumentText { get => _text; set { _text = value ?? ""; _scroll = 0; Invalidate(); } }
    public string Url { get; private set; } = "";
    public int VerticalScroll { get => _scroll; set { _scroll = value < 0 ? 0 : value; Invalidate(); } }

    /// <summary>Fetch an http(s) URL (or read a local file) and render its text.</summary>
    public void Navigate(string url)
    {
        Url = url ?? "";
        try
        {
            if (string.IsNullOrEmpty(url)) { DocumentText = ""; return; }
            if (url.StartsWith("http://", StringComparison.OrdinalIgnoreCase) ||
                url.StartsWith("https://", StringComparison.OrdinalIgnoreCase))
            {
                using var http = new System.Net.Http.HttpClient();
                byte[] bytes = http.GetByteArrayAsync(url).GetAwaiter().GetResult();
                DocumentText = StripHtml(System.Text.Encoding.UTF8.GetString(bytes));
            }
            else
            {
                DocumentText = StripHtml(System.IO.File.ReadAllText(url));
            }
        }
        catch (Exception ex) { DocumentText = "Unable to load " + url + ": " + ex.Message; }
    }

    public void NavigateToString(string html) { DocumentText = StripHtml(html ?? ""); }

    private static string StripHtml(string html)
    {
        string t = System.Text.RegularExpressions.Regex.Replace(html, "<script[\\s\\S]*?</script>", " ",
            System.Text.RegularExpressions.RegexOptions.IgnoreCase);
        t = System.Text.RegularExpressions.Regex.Replace(t, "<[^>]*>", " ");
        t = System.Net.WebUtility.HtmlDecode(t);
        t = System.Text.RegularExpressions.Regex.Replace(t, "[ \\t]+", " ");
        t = System.Text.RegularExpressions.Regex.Replace(t, "\n\\s*\n+", "\n\n");
        return t.Trim();
    }

    internal override void PaintSelf(Graphics g)
    {
        g.FillRectangle(BackColor, new Rectangle(0, 0, Width, Height));
        int lineH = (int)g.MeasureString("Ag", Font).Height + 2;
        if (lineH < 4) lineH = 14;
        int y = -_scroll;
        foreach (string line in Wrap(g, _text, Width - 8))
        {
            if (y + lineH >= 0 && y < Height) g.DrawString(line, Font, ForeColor, 4, y);
            y += lineH;
        }
        g.DrawRectangle(new Pen(SystemColors.ControlDark), new Rectangle(0, 0, Width, Height));
        base.PaintSelf(g);
    }

    private List<string> Wrap(Graphics g, string text, int width)
    {
        var lines = new List<string>();
        if (width < 8) width = 8;
        foreach (string para in text.Replace("\r", "").Split('\n'))
        {
            string cur = "";
            foreach (string word in para.Split(' '))
            {
                string cand = cur.Length == 0 ? word : cur + " " + word;
                if (g.MeasureString(cand, Font).Width > width && cur.Length > 0) { lines.Add(cur); cur = word; }
                else cur = cand;
            }
            lines.Add(cur);
        }
        return lines;
    }
}

/// <summary>Desktop GL for managed callers: a compatibility-profile context on
/// a form's window DC, presented with SwapBuffers (EGL pbuffer readback in the
/// shared library). Load extra entry points with GetProcAddress.</summary>
public sealed class GlContext : IDisposable
{
    private readonly IntPtr _dc;
    private IntPtr _rc;

    public GlContext(Form form)
    {
        if (form == null) throw new ArgumentNullException(nameof(form));
        form.CreateHandle();
        _dc = Native.GetDC(form.Handle);
        _rc = Native.wglCreateContext(_dc);
        if (_rc != IntPtr.Zero) Native.wglMakeCurrent(_dc, _rc);
    }

    public bool IsCurrent => _rc != IntPtr.Zero;
    public IntPtr Dc => _dc;
    public string Renderer => IsCurrent ? (Marshal.PtrToStringAnsi(Native.glGetString(0x1F01)) ?? "") : "";
    public string Version => IsCurrent ? (Marshal.PtrToStringAnsi(Native.glGetString(0x1F02)) ?? "") : "";
    public IntPtr GetProcAddress(string name) => Native.wglGetProcAddress(name);
    public void Swap() { if (_rc != IntPtr.Zero) Native.SwapBuffers(_dc); }

    public void Dispose()
    {
        if (_rc != IntPtr.Zero) { Native.wglDeleteContext(_rc); _rc = IntPtr.Zero; }
    }
}
}
