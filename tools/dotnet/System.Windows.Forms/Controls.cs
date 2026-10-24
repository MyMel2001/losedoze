// Controls.cs — the WinForms control hierarchy for losedoze's managed desktop:
// Control (bounds/events/hit-test/focus), Form (native top-level window over the
// Win32 layer) and the basic controls (Button/Label/TextBox/CheckBox/RadioButton/
// ListBox/Panel), drawn owner-draw by the form. Namespace System.Windows.Forms.
using System;
using System.Collections;
using System.Collections.Generic;
using System.Drawing;

namespace System.Windows.Forms {

public enum Keys { None = 0, Back = 0x08, Tab = 0x09, Enter = 0x0D, Escape = 0x1B, Space = 0x20, PageUp = 0x21, PageDown = 0x22, End = 0x23, Home = 0x24, Left = 0x25, Up = 0x26, Right = 0x27, Down = 0x28, Delete = 0x2E }

public enum MouseButtons { None = 0, Left = 1, Right = 2, Middle = 4 }
public enum ContentAlignment { TopLeft, TopCenter, TopRight, MiddleLeft, MiddleCenter, MiddleRight, BottomLeft, BottomCenter, BottomRight }
public enum DialogResult { None = 0, OK = 1, Cancel = 2, Abort = 3, Retry = 4, Ignore = 5, Yes = 6, No = 7 }
public enum MessageBoxButtons { OK = 0, OKCancel = 1, YesNo = 2, YesNoCancel = 3 }
public enum AutoScaleMode { None = 0, Font = 1, Dpi = 2, Inherit = -1 }

public class LayoutEventArgs : System.EventArgs
{
    public LayoutEventArgs(Rectangle affectedBounds) { AffectedBounds = affectedBounds; }
    public Rectangle AffectedBounds { get; }
}

public class PaintEventArgs : System.EventArgs
{
    public PaintEventArgs(Graphics graphics, Rectangle clipRectangle) { Graphics = graphics; ClipRectangle = clipRectangle; }
    public Graphics Graphics { get; }
    public Rectangle ClipRectangle { get; }
}

public class MouseEventArgs : System.EventArgs
{
    public MouseEventArgs(MouseButtons button, int clicks, int x, int y) { Button = button; Clicks = clicks; X = x; Y = y; }
    public MouseButtons Button { get; }
    public int Clicks { get; }
    public int X { get; }
    public int Y { get; }
}

public class KeyEventArgs : System.EventArgs
{
    public KeyEventArgs(Keys keyData) { KeyData = keyData; }
    public Keys KeyData { get; }
    public Keys KeyCode => KeyData;
}

public class KeyPressEventArgs : System.EventArgs
{
    public KeyPressEventArgs(char keyChar) { KeyChar = keyChar; }
    public char KeyChar { get; }
    public bool Handled { get; set; }
}

public delegate void PaintEventHandler(object? sender, PaintEventArgs e);
public delegate void MouseEventHandler(object? sender, MouseEventArgs e);
public delegate void KeyEventHandler(object? sender, KeyEventArgs e);
public delegate void KeyPressEventHandler(object? sender, KeyPressEventArgs e);

// ---------------------------------------------------------------- Control

public class Control
{
    private Control? _parent;
    private readonly ControlCollection _children = new ControlCollection();
    private string _text = "";

    internal Form? Owner;

    public Control()
    {
        Visible = true;
        Enabled = true;
        Width = 100;
        Height = 20;
    }

    public int Left { get; set; }
    public int Top { get; set; }
    public int Width { get; set; }
    public int Height { get; set; }
    public Control? Parent => _parent;

    public string Name { get; set; } = "";
    public virtual string Text { get => _text; set { _text = value ?? ""; Invalidate(); } }
    public Color BackColor { get; set; } = SystemColors.Control;
    public Color ForeColor { get; set; } = SystemColors.ControlText;
    public System.Drawing.Font Font { get; set; } = new Font();
    public bool Visible { get; set; }
    public bool Enabled { get; set; }
    public bool AutoSize { get; set; }

    public bool Focused => Owner != null && ReferenceEquals(Owner.FocusedControl, this);

    public ControlCollection Controls => _children;

    public Rectangle Bounds => new Rectangle(Left, Top, Width, Height);
    public Size ClientSize { get => new Size(Width, Height); set { Width = value.Width; Height = value.Height; } }
    public System.Drawing.Size Size { get => new Size(Width, Height); set { Width = value.Width; Height = value.Height; } }
    public System.Drawing.Point Location => new Point(Left, Top);
    public int TabIndex { get; set; }
    public bool TabStop { get; set; } = true;

    /* Designer / generated-code-behind lifecycle (InitializeComponent calls
     * SuspendLayout, sets properties, then ResumeLayout/PerformLayout). */
    private bool _layoutSuspended;
    public AutoScaleMode AutoScaleMode { get; set; } = AutoScaleMode.None;
    public SizeF AutoScaleDimensions { get; set; }
    public SizeF AutoScaleFactor => new SizeF(1f, 1f);
    public event EventHandler<LayoutEventArgs>? Layout;
    public void SuspendLayout() { _layoutSuspended = true; }
    public void ResumeLayout(bool performLayout = true)
    {
        bool was = _layoutSuspended;
        _layoutSuspended = false;
        if (performLayout && was) PerformLayout();
    }
    public void PerformLayout() { if (_layoutSuspended) return; OnLayout(new LayoutEventArgs(Bounds)); }
    protected virtual void OnLayout(LayoutEventArgs e) { Layout?.Invoke(this, e); }

    public void Add(Control c) { _children.Add(c); }
    public void AddRange(params Control[] controls) { foreach (var c in controls) Add(c); }
    public void Remove(Control c) { _children.Remove(c); }
    public void Clear() { _children.Clear(); }

    public virtual void Invalidate() { Owner?.Invalidate(); }
    public void Focus() { Owner?.SetFocus(this); }

    internal void SetParent(Control? p) { _parent = p; SetOwner(p?.Owner); }
    private void SetOwner(Form? f) { Owner = f; foreach (var c in _children) c.SetOwner(f); }

    public event PaintEventHandler? Paint;
    public event MouseEventHandler? MouseDown;
    public event MouseEventHandler? MouseUp;
    public event MouseEventHandler? MouseMove;
    public event KeyEventHandler? KeyDown;
    public event KeyPressEventHandler? KeyPress;
    public event EventHandler? Click;
    public event EventHandler? TextChanged;

    internal virtual void PaintSelf(Graphics g)
    {
        var e = new PaintEventArgs(g, Bounds);
        OnPaint(e);
        Paint?.Invoke(this, e);
    }

    protected virtual void OnPaint(PaintEventArgs e) { }
    protected virtual void OnClick(EventArgs e) { }
    protected virtual void OnMouseDown(MouseEventArgs e) { }
    protected virtual void OnMouseUp(MouseEventArgs e) { }
    protected virtual void OnMouseMove(MouseEventArgs e) { }
    protected virtual void OnKeyDown(KeyEventArgs e) { }
    protected virtual void OnKeyPress(KeyPressEventArgs e) { }

    internal virtual void RaiseClick() { OnClick(System.EventArgs.Empty); Click?.Invoke(this, System.EventArgs.Empty); Invalidate(); }
    internal virtual void RaiseMouseDown(int x, int y, MouseButtons b) { var e = new MouseEventArgs(b, 1, x, y); OnMouseDown(e); MouseDown?.Invoke(this, e); }
    internal virtual void RaiseMouseUp(int x, int y, MouseButtons b) { var e = new MouseEventArgs(b, 1, x, y); OnMouseUp(e); MouseUp?.Invoke(this, e); }
    internal virtual void RaiseMouseMove(int x, int y, MouseButtons b) { var e = new MouseEventArgs(b, 0, x, y); OnMouseMove(e); MouseMove?.Invoke(this, e); }

    internal virtual bool HandleKeyDown(Keys keyCode) { var e = new KeyEventArgs(keyCode); OnKeyDown(e); KeyDown?.Invoke(this, e); return false; }
    internal virtual bool HandleKeyPress(char ch) { var e = new KeyPressEventArgs(ch); OnKeyPress(e); KeyPress?.Invoke(this, e); return e.Handled; }

    protected void OnTextChanged(EventArgs e) { TextChanged?.Invoke(this, e); }

    internal Control HitTest(int x, int y, out int lx, out int ly)
    {
        lx = x; ly = y;
        if (!Visible) return this;
        for (int i = _children.Count - 1; i >= 0; i--)
        {
            var c = _children[i];
            if (!c.Visible || !c.Enabled) continue;
            if (x >= c.Left && x < c.Left + c.Width && y >= c.Top && y < c.Top + c.Height)
                return c.HitTest(x - c.Left, y - c.Top, out lx, out ly);
        }
        return this;
    }

    internal virtual void PaintTree(Graphics g)
    {
        if (!Visible) return;
        PaintSelf(g);
        foreach (var c in _children)
        {
            if (!c.Visible) continue;
            /* owner-drawn children must not bleed outside their bounds */
            int saved = System.Windows.Forms.Native.SaveDC(g.Hdc);
            var clip = new System.Windows.Forms.Native.RECT
            {
                left = g.Ox + c.Left, top = g.Oy + c.Top,
                right = g.Ox + c.Left + c.Width, bottom = g.Oy + c.Top + c.Height,
            };
            System.Windows.Forms.Native.IntersectClipRect(g.Hdc, clip.left, clip.top, clip.right, clip.bottom);
            g.Ox += c.Left; g.Oy += c.Top;
            c.PaintTree(g);
            g.Ox -= c.Left; g.Oy -= c.Top;
            System.Windows.Forms.Native.RestoreDC(g.Hdc, saved);
        }
    }
}

public class ControlCollection : IEnumerable
{
    private readonly Control _owner;
    private readonly List<Control> _items = new List<Control>();
    internal ControlCollection(Control owner) { _owner = owner; }
    public int Count => _items.Count;
    public Control this[int i] => _items[i];
    public void Add(Control c) { if (!_items.Contains(c)) { c.SetParent(_owner); _items.Add(c); } }
    public void AddRange(params Control[] controls) { foreach (var c in controls) Add(c); }
    public void Remove(Control c) { _items.Remove(c); }
    public void Clear() { foreach (var c in _items) c.SetParent(null); _items.Clear(); }
    public IEnumerator GetEnumerator() => _items.GetEnumerator();
}

// ---------------------------------------------------------------- Form

public class Form : Control
{
    private readonly Native.WndProc _proc;
    private readonly string _cls;
    private IntPtr _hwnd;
    private Control? _focused;
    private Control? _mouseDown;
    private bool _loaded;
    private bool _modal, _loopExit;
    private DialogResult _result;

    public Form()
    {
        _proc = WndProcImpl;
        _cls = "LosedozeWinForms_" + Guid.NewGuid().ToString("N")[..8];
        Owner = this;
    }

    public IntPtr Handle => _hwnd;
    public bool IsHandleCreated => _hwnd != IntPtr.Zero;
    private volatile int _paints;
    public int Paints => _paints;
    public string Title { get; set; } = "";
    /// <summary>WinForms designer code sets Text; the Win32 layer wants a Title.</summary>
    public override string Text { get => Title; set => Title = value ?? ""; }

    internal Control? FocusedControl => _focused;

    public event EventHandler? Load;
    public event EventHandler? Shown;
    public event EventHandler? Closing;
    public event EventHandler? Closed;

    public override void Invalidate()
    {
        if (_hwnd == IntPtr.Zero) return;
        var r = new Native.RECT();
        Native.InvalidateRect(_hwnd, ref r, 1);
    }

    public void SetFocus(Control c) { _focused = c; Invalidate(); }

    public void CreateHandle()
    {
        if (_hwnd != IntPtr.Zero) return;
        var wc = new Native.WNDCLASSW { lpfnWndProc = _proc, lpszClassName = _cls };
        Native.RegisterClassExW(ref wc);
        _hwnd = Native.CreateWindowExW(0, _cls, Title,
            Native.WS_OVERLAPPEDWINDOW | Native.WS_VISIBLE, 0, 0, Width, Height,
            IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);
    }

    public void Show() { CreateHandle(); }
    public void Close() { if (_hwnd != IntPtr.Zero) Native.PostMessageW(_hwnd, Native.WM_CLOSE, 0, 0); }
    public DialogResult DialogResult { get; set; } = DialogResult.None;

    /// <summary>Show modally: runs a nested message loop until Close()/the
    /// dialog sets DialogResult. Returns DialogResult.</summary>
    public DialogResult ShowDialog()
    {
        CreateHandle();
        _modal = true;
        _loopExit = false;
        _result = DialogResult.None;
        RunLoop();
        _modal = false;
        return _result;
    }

    internal void RunLoop()
    {
        if (!_loaded) { _loaded = true; OnLoad(System.EventArgs.Empty); Load?.Invoke(this, System.EventArgs.Empty); }
        Shown?.Invoke(this, System.EventArgs.Empty);
        var m = new Native.MSG();
        while (Native.GetMessageW(ref m, IntPtr.Zero, 0, 0) > 0)
        {
            if (m.message == Native.WM_QUIT || _loopExit) break;
            Native.TranslateMessage(ref m);
            Native.DispatchMessageW(ref m);
            if (_loopExit) break;
        }
        Closed?.Invoke(this, System.EventArgs.Empty);
        OnClosed(System.EventArgs.Empty);
    }

    public void Run() { CreateHandle(); RunLoop(); }

    protected virtual void OnLoad(EventArgs e) { }
    protected virtual void OnClosed(EventArgs e) { }

    protected virtual void OnPaintFrame(Graphics g)
    {
        g.FillRectangle(BackColor, new Rectangle(0, 0, Width, Height));
        PaintTree(g);
    }

    private IntPtr WndProcImpl(IntPtr hwnd, uint msg, ulong wp, long lp)
    {
        switch (msg)
        {
            case Native.WM_PAINT:
            {
                var ps = new Native.PAINTSTRUCT { rgbReserved = new byte[32] };
                IntPtr dc = Native.BeginPaint(hwnd, ref ps);
                var g = new Graphics(dc);
                OnPaintFrame(g);
                Native.EndPaint(hwnd, ref ps);
                _paints++;
                break;
            }
            case Native.WM_LBUTTONDOWN:
            case Native.WM_RBUTTONDOWN:
            case Native.WM_LBUTTONUP:
            case Native.WM_RBUTTONUP:
            case Native.WM_MOUSEMOVE:
            {
                int x = (int)(lp & 0xffff);
                int y = (int)((lp >> 16) & 0xffff);
                var t = HitTest(x, y, out int lx, out int ly);
                if (msg == Native.WM_LBUTTONDOWN || msg == Native.WM_RBUTTONDOWN)
                {
                    var btn = msg == Native.WM_RBUTTONDOWN ? MouseButtons.Right : MouseButtons.Left;
                    _mouseDown = t;
                    SetFocus(t);
                    t.RaiseMouseDown(lx, ly, btn);
                }
                else if (msg == Native.WM_LBUTTONUP || msg == Native.WM_RBUTTONUP)
                {
                    var btn = msg == Native.WM_RBUTTONUP ? MouseButtons.Right : MouseButtons.Left;
                    t.RaiseMouseUp(lx, ly, btn);
                    if (t == _mouseDown) t.RaiseClick();
                    _mouseDown = null;
                }
                else t.RaiseMouseMove(lx, ly, MouseButtons.None);
                break;
            }
            case Native.WM_SIZE:
            {
                /* compositor resized the toplevel: adopt the new client size */
                int cx = (int)((wp >> 16) & 0xffff);
                int cy = (int)(wp & 0xffff);
                Width = cx;
                Height = cy;
                Invalidate();
                break;
            }
            case Native.WM_CHAR:
            {
                char ch = (char)(wp & 0xff);
                var t = _focused ?? this;
                if (!t.HandleKeyPress(ch) && !ReferenceEquals(t, this)) HandleKeyPress(ch);
                break;
            }
            case Native.WM_KEYDOWN:
            {
                var k = (Keys)(wp & 0xffff);
                var t = _focused ?? this;
                if (!t.HandleKeyDown(k) && !ReferenceEquals(t, this)) HandleKeyDown(k);
                break;
            }
            case Native.WM_CLOSE:
                Closing?.Invoke(this, System.EventArgs.Empty);
                Native.DestroyWindow(hwnd);
                if (_modal) { _result = DialogResult; _loopExit = true; }
                else Native.PostQuitMessage(0);
                return IntPtr.Zero;
        }
        return Native.DefWindowProcW(hwnd, msg, wp, lp);
    }
}
}
