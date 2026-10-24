// Application.cs — the WinForms application object, MessageBox and the system
// palette, over the losedoze Win32 layer (libwin32.so). No Wine, no Microsoft
// Windows Desktop runtime: this is our own WinForms-shaped framework.
using System;
using System.Drawing;

namespace System.Windows.Forms {

public static class SystemColors
{
    public static Color Control => Color.FromArgb(0x1f, 0x1f, 0x1f);
    public static Color ControlText => Color.FromArgb(0xff, 0xff, 0xff);
    public static Color ControlDark => Color.FromArgb(0x5a, 0x5a, 0x5a);
    public static Color ControlLight => Color.FromArgb(0x2b, 0x2b, 0x2b);
    public static Color Window => Color.FromArgb(0xff, 0xff, 0xff);
    public static Color WindowText => Color.FromArgb(0x10, 0x10, 0x10);
    public static Color Highlight => Color.FromArgb(0x2f, 0x6f, 0xed);
    public static Color HighlightText => Color.FromArgb(0xff, 0xff, 0xff);
    public static Color GrayText => Color.FromArgb(0x9a, 0x9a, 0x9a);
    public static Color ButtonFace => Color.FromArgb(0x3a, 0x3a, 0x3a);
    public static Color ButtonText => Color.FromArgb(0xff, 0xff, 0xff);
    public static Color Info => Color.FromArgb(0x2b, 0x2b, 0x2b);
}

public static class Application
{
    private static bool _initialized;
    private static bool _connected;

    /// <summary>True when the Win32 layer connected to the losedoze compositor.</summary>
    public static bool CompositorConnected => _connected;

    public static void EnableVisualStyles() { }
    public static void SetCompatibleTextRenderingDefault(bool useTextRenderer) { }
    public static void SetHighDpiMode(int mode) { }

    /// <summary>Start the Win32 layer (idempotent). Called by Run/DoEvents.</summary>
    public static void Init()
    {
        if (_initialized) return;
        _initialized = true;
        _connected = Native.los_init() != 0;
    }

    public static void Run(Form form)
    {
        if (form == null) throw new ArgumentNullException(nameof(form));
        Init();
        form.CreateHandle();
        form.RunLoop();
    }

    public static void Run()
    {
        Init();
        var m = new Native.MSG();
        while (Native.GetMessageW(ref m, IntPtr.Zero, 0, 0) > 0)
        {
            if (m.message == Native.WM_QUIT) break;
            Native.TranslateMessage(ref m);
            Native.DispatchMessageW(ref m);
        }
    }

    public static void DoEvents()
    {
        Init();
        var m = new Native.MSG();
        while (Native.PeekMessageW(ref m, IntPtr.Zero, 0, 0, 1) != 0)
        {
            if (m.message == Native.WM_QUIT) break;
            Native.TranslateMessage(ref m);
            Native.DispatchMessageW(ref m);
        }
    }

    public static void Exit() { Native.PostQuitMessage(0); }
    public static void ExitThread() { Native.PostQuitMessage(0); }
}

public static class MessageBox
{
    public static DialogResult Show(string text) => Show(text, "", MessageBoxButtons.OK);

    public static DialogResult Show(string text, string caption) => Show(text, caption, MessageBoxButtons.OK);

    public static DialogResult Show(string text, string caption, MessageBoxButtons buttons)
    {
        Application.Init();
        if (!Application.CompositorConnected)
            return Native.MessageBoxW(IntPtr.Zero, text ?? "", caption ?? "", 0) == Native.IDOK
                ? DialogResult.OK : DialogResult.Cancel;   /* console-only: logged, auto-OK */

        var form = new Form { Title = caption ?? "", Width = 380, Height = 170 };
        form.Controls.Add(new Label { Text = text ?? "", Left = 16, Top = 16, Width = 340, Height = 56 });
        var result = DialogResult.OK;
        int x = 16;
        void AddButton(string label, DialogResult r)
        {
            var b = new Button { Left = x, Top = 88, Width = 96, Height = 30, Text = label };
            b.Click += (s, e) => { result = r; form.DialogResult = r; form.Close(); };
            form.Controls.Add(b);
            x += 102;
        }
        switch (buttons)
        {
            case MessageBoxButtons.OKCancel: AddButton("OK", DialogResult.OK); AddButton("Cancel", DialogResult.Cancel); break;
            case MessageBoxButtons.YesNo: AddButton("Yes", DialogResult.Yes); AddButton("No", DialogResult.No); break;
            case MessageBoxButtons.YesNoCancel: AddButton("Yes", DialogResult.Yes); AddButton("No", DialogResult.No); AddButton("Cancel", DialogResult.Cancel); break;
            default: AddButton("OK", DialogResult.OK); break;
        }
        form.ShowDialog();
        return result;
    }
}
}
