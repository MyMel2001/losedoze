// Program.cs — losedoze WinForms demo app.
//
// First managed app written against the losedoze WinForms framework
// (System.Windows.Forms + System.Drawing namespaces, implemented on top of the
// homemade Win32 layer / libwin32.so): a real window in the compositor, painted
// by managed controls over GDI, run through Application.Run.
//
// Marker: LOSEDOZE WINFORMS OK <version> — printed after the first WM_PAINT
// reached the compositor buffer.
using System;
using System.Drawing;
using System.Windows.Forms;

internal static class Program
{
    private static int Main()
    {
        var form = new Form { Title = "Losedoze WinForms", Width = 560, Height = 440 };
        var lblTitle = new Label { Text = "Losedoze WinForms", Width = 512, Height = 30, Left = 24, Top = 16, TextAlign = ContentAlignment.MiddleCenter, ForeColor = SystemColors.Highlight };
        var lblSub = new Label { Text = "managed WinForms over the homemade Win32 layer", Width = 512, Height = 20, Left = 24, Top = 50, TextAlign = ContentAlignment.MiddleCenter };
        var txt = new TextBox { Left = 24, Top = 88, Width = 220, Height = 26, Text = "hello winforms" };
        var lblLen = new Label { Text = "text length: 0", Left = 24, Top = 122, Width = 220, Height = 20 };
        var check = new CheckBox { Text = "show border", Left = 24, Top = 150, Width = 160, Height = 22 };
        var panel = new Panel { Left = 260, Top = 150, Width = 260, Height = 90 };
        panel.Paint += (s, e) => { var g = e.Graphics; g.FillRectangle(SystemColors.ControlLight, new Rectangle(0, 0, panel.Width, panel.Height)); g.DrawRectangle(new Pen(SystemColors.ControlDark), new Rectangle(0, 0, panel.Width, panel.Height)); g.DrawString(panel.Text, panel.Font, SystemColors.WindowText, 10, 10); };
        var btn = new Button { Left = 24, Top = 200, Width = 120, Height = 30, Text = "Click me" };
        var lblClicks = new Label { Text = "clicks: 0", Left = 152, Top = 208, Width = 120, Height = 20 };
        var grid = new DataGridView { Left = 260, Top = 88, Width = 260, Height = 56 };
        grid.AddRow("cell", "grid"); grid.AddRow("managed", "C#"); grid.AddRow("owner", "drawn");

        form.Controls.Add(lblTitle);
        form.Controls.Add(lblSub);
        form.Controls.Add(txt);
        form.Controls.Add(lblLen);
        form.Controls.Add(check);
        form.Controls.Add(panel);
        form.Controls.Add(btn);
        form.Controls.Add(lblClicks);
        form.Controls.Add(grid);

        // desktop GL for managed callers: context on the window DC (EGL-backed)
        form.Load += (s, e) =>
        {
            using var gl = new GlContext(form);
            Console.WriteLine("los: GL " + (gl.IsCurrent ? gl.Renderer : "none"));
            Console.Out.Flush();
        };

        int clicks = 0;
        btn.Click += (s, e) => { clicks++; btn.Text = "Clicks: " + clicks; lblClicks.Text = "clicks: " + clicks; btn.Invalidate(); };

        txt.TextChanged += (s, e) => lblLen.Text = "text length: " + txt.Text.Length;
        check.CheckedChanged += (s, e) => check.Invalidate();
        form.KeyDown += (s, e) => { if (e.KeyData == Keys.Escape) form.Close(); };

        var watcher = new System.Threading.Thread(() =>
        {
            for (int i = 0; i < 400 && form.Paints == 0; i++)
                System.Threading.Thread.Sleep(50);
            Console.WriteLine(form.Paints > 0
                ? "LOSEDOZE WINFORMS OK " + Environment.Version
                : "LOSEDOZE WINFORMS FAIL (no paint)");
            Console.Out.Flush();
        });
        watcher.IsBackground = true;
        watcher.Start();

        Application.Run(form);
        return 0;
    }
}
