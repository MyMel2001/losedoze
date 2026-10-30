// Program.cs — Losedoze .NET runtime probe.
//
// Minimal managed probe: boots the guest CoreCLR runtime and exits. The smoke
// console marker contract (`LOSEDOZE DOTNET OK`) comes from this app; the heavier
// managed desktop apps (Lopedoze.Desktop / Lopedoze.WinForms) run later in the
// suite and print their own markers.
using System;

class Program
{
    static int Main()
    {
        Console.WriteLine("LOSEDOZE DOTNET OK " + Environment.Version);
        Console.Out.Flush();
        return 0;
    }
}
