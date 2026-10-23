// BasicControls.cs — the first wave of WinForms-shaped controls for losedoze's
// managed desktop: Button/Label/TextBox/CheckBox/RadioButton/ListBox/Panel.
// They are owner-drawn by their (top-level) parent Form over the GDI canvas;
// hit-testing lives in Control.HitTest, input in the WM_* routing in Form.
using System;
using System.Collections.Generic;
using System.Drawing;

namespace System.Windows.Forms {

public class Button : Control
{
    public Button() { Width = 88; Height = 28; Text = "Button"; }

    internal override void PaintSelf(Graphics g)
    {
        var r = new Rectangle(0, 0, Width, Height);
        g.FillRectangle(SystemColors.ButtonFace, r);
        g.DrawRectangle(new Pen(Focused ? SystemColors.Highlight : SystemColors.ControlDark), r);
        var sz = g.MeasureString(Text, Font);
        int tx = (int)((Width - sz.Width) / 2); if (tx < 2) tx = 2;
        int ty = (int)((Height - sz.Height) / 2); if (ty < 2) ty = 2;
        g.DrawString(Text, Font, SystemColors.ButtonText, tx, ty);
        base.PaintSelf(g);
    }
}

public class Label : Control
{
    public Label() { BackColor = Color.Transparent; Width = 100; Height = 20; Text = ""; }
    public ContentAlignment TextAlign { get; set; } = ContentAlignment.MiddleLeft;

    internal override void PaintSelf(Graphics g)
    {
        var sz = g.MeasureString(Text, Font);
        int y = (Height - (int)sz.Height) / 2; if (y < 0) y = 0;
        g.DrawString(Text, Font, ForeColor, 0, y);
        base.PaintSelf(g);
    }
}

public class TextBox : Control
{
    private int _caret;
    public TextBox() { BackColor = SystemColors.Window; ForeColor = SystemColors.WindowText; Height = 26; Width = 160; Text = ""; }

    public bool ReadOnly { get; set; }
    public bool Multiline { get; set; }
    public int MaxLength { get; set; } = 32767;
    public int SelectionStart { get => _caret; set { _caret = Math.Max(0, Math.Min(value, Text.Length)); Invalidate(); } }

    public override string Text { get => base.Text; set { base.Text = value ?? ""; if (_caret > base.Text.Length) _caret = base.Text.Length; } }

    internal override void PaintSelf(Graphics g)
    {
        g.FillRectangle(BackColor, new Rectangle(0, 0, Width, Height));
        g.DrawRectangle(new Pen(Focused ? SystemColors.Highlight : SystemColors.ControlDark), new Rectangle(0, 0, Width, Height));
        g.DrawString(Text, Font, ForeColor, 5, 5);
        if (Focused && !ReadOnly)
        {
            int cx = 5 + (int)g.MeasureString(Text.Substring(0, Math.Min(_caret, Text.Length)), Font).Width;
            g.DrawLine(new Pen(SystemColors.WindowText), cx, 5, cx, Height - 6);
        }
        base.PaintSelf(g);
    }

    internal override bool HandleKeyPress(char ch)
    {
        if (ch < ' ' || ReadOnly) return true;   // control chars come as WM_KEYDOWN, not edit here
        if (Text.Length >= MaxLength) return true;
        Text = Text.Insert(Math.Min(_caret, Text.Length), ch.ToString());
        _caret++;
        OnTextChanged(System.EventArgs.Empty);
        return true;
    }

    internal override bool HandleKeyDown(Keys k)
    {
        switch (k)
        {
            case Keys.Left: if (_caret > 0) { _caret--; Invalidate(); } return true;
            case Keys.Right: if (_caret < Text.Length) { _caret++; Invalidate(); } return true;
            case Keys.Home: _caret = 0; Invalidate(); return true;
            case Keys.End: _caret = Text.Length; Invalidate(); return true;
            case Keys.Back: if (!ReadOnly && _caret > 0) { Text = Text.Remove(_caret - 1, 1); _caret--; OnTextChanged(System.EventArgs.Empty); } return true;
            case Keys.Delete: if (!ReadOnly && _caret < Text.Length) { Text = Text.Remove(_caret, 1); OnTextChanged(System.EventArgs.Empty); } return true;
            case Keys.Enter: return false;
            case Keys.Tab: return false;
        }
        return base.HandleKeyDown(k);
    }
}

public class CheckBox : Control
{
    public CheckBox() { BackColor = Color.Transparent; Width = 160; Height = 22; }

    public bool Checked { get; set; }

    internal override void PaintSelf(Graphics g)
    {
        var box = new Rectangle(2, (Height - 16) / 2, 16, 16);
        g.FillRectangle(SystemColors.Window, box);
        g.DrawRectangle(new Pen(SystemColors.ControlDark), box);
        if (Checked)
        {
            var pen = new Pen(SystemColors.Highlight, 2);
            g.DrawLine(pen, box.Left + 4, box.Top + 8, box.Left + 7, box.Top + 11);
            g.DrawLine(pen, box.Left + 7, box.Top + 11, box.Left + 12, box.Top + 4);
        }
        g.DrawString(Text, Font, ForeColor, 24, (Height - 16) / 2);
        base.PaintSelf(g);
    }

    public event EventHandler? CheckedChanged;

    internal override void RaiseClick() { Checked = !Checked; CheckedChanged?.Invoke(this, System.EventArgs.Empty); base.RaiseClick(); }
}

public class RadioButton : Control
{
    public RadioButton() { Width = 160; Height = 22; BackColor = Color.Transparent; }

    public bool Checked { get; set; }

    internal override void PaintSelf(Graphics g)
    {
        var box = new Rectangle(2, (Height - 16) / 2, 16, 16);
        g.FillRectangle(SystemColors.Window, box);
        g.DrawRectangle(new Pen(SystemColors.ControlDark), box);
        if (Checked)
        {
            g.FillRectangle(SystemColors.Highlight, new Rectangle(box.Left + 5, box.Top + 5, 6, 6));
        }
        g.DrawString(Text, Font, ForeColor, 24, (Height - 16) / 2);
        base.PaintSelf(g);
    }

    public event EventHandler? CheckedChanged;

    internal override void RaiseClick()
    {
        Checked = true;
        if (Parent != null)
            foreach (Control c in Parent.Controls)
                if (c is RadioButton rb && !ReferenceEquals(rb, this)) rb.Checked = false;
        CheckedChanged?.Invoke(this, System.EventArgs.Empty);
        base.RaiseClick();
    }
}

public class ListBox : Control
{
    public ListBox() { BackColor = SystemColors.Window; Width = 160; Height = 120; }

    public List<string> Items { get; } = new List<string>();
    public int SelectedIndex { get; set; } = -1;

    public string? SelectedItem => SelectedIndex >= 0 && SelectedIndex < Items.Count ? Items[SelectedIndex] : null;

    internal override void PaintSelf(Graphics g)
    {
        g.FillRectangle(BackColor, new Rectangle(0, 0, Width, Height));
        g.DrawRectangle(new Pen(SystemColors.ControlDark), new Rectangle(0, 0, Width, Height));
        int y = 2;
        for (int i = 0; i < Items.Count; i++)
        {
            if (i == SelectedIndex) g.FillRectangle(SystemColors.Highlight, new Rectangle(2, y, Width - 4, 18));
            g.DrawString(Items[i], Font, i == SelectedIndex ? SystemColors.HighlightText : SystemColors.WindowText, 6, y + 1);
            y += 18;
            if (y > Height) break;
        }
        base.PaintSelf(g);
    }

    internal override void RaiseMouseDown(int x, int y, MouseButtons b)
    {
        int i = (y - 2) / 18;
        if (i >= 0 && i < Items.Count && i != SelectedIndex) { SelectedIndex = i; Invalidate(); }
        base.RaiseMouseDown(x, y, b);
    }
}

public class Panel : Control
{
    public Panel() { BackColor = SystemColors.Control; }

    internal override void PaintSelf(Graphics g) { g.FillRectangle(BackColor, new Rectangle(0, 0, Width, Height)); base.PaintSelf(g); }
}
}
