// Dialogs.cs — owner-drawn modal file dialogs for losedoze's managed WinForms:
// OpenFileDialog / SaveFileDialog / FolderBrowserDialog over a Form with a
// ListBox, a path TextBox and OK/Cancel buttons (real modal loop).
using System;
using System.Drawing;
using System.IO;

namespace System.Windows.Forms {

internal sealed class FileDialogForm
{
    public string Title = "Select";
    public string InitialDirectory = "/tmp";
    public bool FolderOnly;
    public bool SaveMode;
    public string SelectedPath = "";

    private readonly Form _form = new Form();
    private readonly ListBox _list = new ListBox();
    private readonly TextBox _path = new TextBox();
    private string _dir = "/";

    public DialogResult Run()
    {
        _dir = Directory.Exists(InitialDirectory) ? InitialDirectory : "/";
        _form.Title = Title;
        _form.Width = 520; _form.Height = 360;
        _list.Left = 8; _list.Top = 8; _list.Width = 320; _list.Height = 300;
        _path.Left = 336; _path.Top = 8; _path.Width = 170; _path.Height = 26;
        var ok = new Button { Left = 336, Top = 44, Width = 100, Height = 30, Text = SaveMode ? "Save" : (FolderOnly ? "Choose" : "Open") };
        var cancel = new Button { Left = 336, Top = 82, Width = 100, Height = 30, Text = "Cancel" };
        var up = new Button { Left = 336, Top = 120, Width = 100, Height = 30, Text = "Up" };
        _form.Controls.Add(_list); _form.Controls.Add(_path);
        _form.Controls.Add(ok); _form.Controls.Add(cancel); _form.Controls.Add(up);
        ok.Click += (s, e) => Accept();
        cancel.Click += (s, e) => { _form.DialogResult = DialogResult.Cancel; _form.Close(); };
        up.Click += (s, e) => GoUp();
        _list.Click += (s, e) => { if (_list.SelectedItem is string item) _path.Text = item; };
        _form.SetFocus(_list);
        Populate();
        return _form.ShowDialog();
    }

    private void Populate()
    {
        _list.Items.Clear();
        _path.Text = _dir;
        try
        {
            if (Directory.GetParent(_dir) != null) _list.Items.Add("..");
            foreach (var d in Directory.GetDirectories(_dir)) _list.Items.Add(d);
            if (!FolderOnly) foreach (var f in Directory.GetFiles(_dir)) _list.Items.Add(f);
        }
        catch (Exception ex) { _list.Items.Add("(error: " + ex.Message + ")"); }
        _list.SelectedIndex = -1;
    }

    private void GoUp()
    {
        var p = Directory.GetParent(_dir);
        if (p != null) { _dir = p.FullName; Populate(); }
    }

    private void Accept()
    {
        string sel = _list.SelectedItem as string ?? _path.Text;
        if (string.IsNullOrEmpty(sel)) return;
        if (sel == "..") { GoUp(); return; }
        if (Directory.Exists(sel))
        {
            if (FolderOnly) { SelectedPath = sel; _form.DialogResult = DialogResult.OK; _form.Close(); return; }
            _dir = sel; Populate(); return;
        }
        SelectedPath = sel;
        _form.DialogResult = DialogResult.OK;
        _form.Close();
    }
}

public class OpenFileDialog
{
    public string Title { get; set; } = "Open File";
    public string Filter { get; set; } = "All Files (*.*)|*.*";
    public string InitialDirectory { get; set; } = "/tmp";
    public string FileName { get; set; } = "";
    public string SafeFileName => FileName.Length == 0 ? "" : Path.GetFileName(FileName);

    protected virtual bool SaveMode => false;

    public DialogResult ShowDialog()
    {
        var d = new FileDialogForm
        {
            Title = Title,
            InitialDirectory = InitialDirectory,
            SaveMode = SaveMode,
        };
        var r = d.Run();
        if (r == DialogResult.OK) FileName = d.SelectedPath;
        return r;
    }
}

public class SaveFileDialog : OpenFileDialog
{
    public SaveFileDialog() { Title = "Save File"; }
    protected override bool SaveMode => true;
}

public class FolderBrowserDialog
{
    public string Description { get; set; } = "Select Folder";
    public string SelectedPath { get; set; } = "";

    public DialogResult ShowDialog()
    {
        var d = new FileDialogForm
        {
            Title = Description,
            InitialDirectory = SelectedPath.Length > 0 ? SelectedPath : "/",
            FolderOnly = true,
        };
        var r = d.Run();
        if (r == DialogResult.OK) SelectedPath = d.SelectedPath;
        return r;
    }
}
}
