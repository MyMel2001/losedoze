// Program.cs — Losedoze .NET desktop app.
//
// A managed app that draws a real desktop window through the losedoze WinForms
// framework (System.Windows.Forms over the homemade Win32 layer, libwin32.so):
// a Form, a Button, GDI painting and Application.Run. No Wine, no Microsoft
// Windows Desktop runtime; the managed side below is the same WinForms API the
// loswinforms demo uses.
//
// Marker: LOSEDOZE .NET DESKTOP OK <runtime> — printed after the first WM_PAINT
// reached the compositor buffer.
using System;
using System.Drawing;
using System.Windows.Forms;

internal static class Program
{
    private static int Main()
    {
        Application.Init();
        Console.WriteLine("los: Win32 layer up (compositor=" + (Application.CompositorConnected ? "yes" : "no") + ")");

        var form = new Form { Title = "Losedoze .NET Desktop", Width = 560, Height = 360 };
        var btn = new Button { Left = 24, Top = 150, Width = 200, Height = 36, Text = "Click me" };
        form.Controls.Add(btn);
        int clicks = 0;
        btn.Click += (s, e) => { clicks++; btn.Invalidate(); };

        var fg = Color.FromArgb(0xff, 0xff, 0xff);
        var dim = Color.FromArgb(0x9a, 0x9a, 0x9a);
        var accent = Color.FromArgb(0x2f, 0x6f, 0xed);
        form.Paint += (s, e) =>
        {
            var g = e.Graphics;
            g.DrawString("Losedoze .NET Desktop", form.Font, fg, 24, 24);
            g.DrawString("managed C# over the Win32 layer (libwin32.so)", form.Font, dim, 24, 52);
            g.DrawString("window, GDI painting and the message loop are native", form.Font, dim, 24, 76);
            g.DrawLine(new Pen(accent), 24, 120, form.Width - 24, 120);
            g.DrawString("paints=" + (form.Paints + 1) + " clicks=" + clicks, form.Font, dim, 24, form.Height - 40);
            g.DrawString(".NET " + Environment.Version, form.Font, dim, form.Width - 240, form.Height - 40);
        };
        form.KeyDown += (s, e) => { if (e.KeyData == Keys.Escape) form.Close(); };

        // the first paint happens in the message loop: announce it from a watcher
        var watcher = new System.Threading.Thread(() =>
        {
            for (int i = 0; i < 400 && form.Paints == 0; i++)
                System.Threading.Thread.Sleep(50);
            Console.WriteLine(form.Paints > 0
                ? "LOSEDOZE .NET DESKTOP OK " + Environment.Version
                : "LOSEDOZE .NET DESKTOP FAIL (no paint)");
            Console.Out.Flush();
        });
        watcher.IsBackground = true;
        watcher.Start();

        Application.Run(form);
        return 0;
    }
}
