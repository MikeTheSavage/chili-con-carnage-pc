// Setup for Chili Con Carnage PC: one program that is the installer (with its payload after it), the
// uninstaller (the same program without the payload, as Uninstall.exe in the game's folder) and the tool
// that makes the installer (--pack).
//
//   Setup.exe                                   the window
//   Setup.exe --silent --iso <file or folder> [--dir <folder>] [--no-hd-loading] [--no-desktop]
//             [--no-start-menu] [--launch] [--log <file>]
//   Setup.exe --screenshot <folder> [--scale 1] [--iso <file>] [--only <start of a name>] [--pad]
//   Setup.exe --test [--iso <file>]             the tests that need no window; exit 0 when all pass
//   Setup.exe --list                            the payload's files: part, size, path
//   Setup.exe --selftest --iso <file> --home <folder>   the window driven by made-up keys and controller
//   Setup.exe --pack --package <folder> [--hd-loading <folder>] [--setup-files <folder>] [--version 0.9.0]
//             [--publisher <name>] [--music-tool setup/ChiliSoundExport.exe] [--music-args <template>] --out <file>
//   --home <folder>                             tests: a stand-in for the user's folders and registry keys
//   Uninstall.exe [--silent] [--remove-saves]
//
// Exit codes: 0 done; 1 cancelled; 2 bad arguments; 3 the disc was refused; 4 the folder is not allowed or
// cannot be written; 5 not enough free space; 6 the installer's data is damaged; 7 the game or the launcher
// runs from the folder; 8 a file could not be written; 9 another error. The log says why.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Windows;
using ChiliLauncher;

[assembly: AssemblyTitle("Chili Con Carnage PC Setup")]
[assembly: AssemblyProduct("Chili Con Carnage PC")]
[assembly: AssemblyDescription("Installs Chili Con Carnage PC for the current user")]
[assembly: System.Runtime.Versioning.TargetFramework(".NETFramework,Version=v4.8", FrameworkDisplayName = ".NET Framework 4.8")]

namespace ChiliSetup
{
    public sealed class Args
    {
        public bool Silent, Test, Pack, SelfTest, Uninstall, RemoveSaves, NoHd, NoDesktop, NoStartMenu, Launch, Pad, Quiet, List;
        public string Iso, Dir, Log, Home, Screenshot, Only, Package, HdLoading, SetupFiles, Out, Version, Publisher, MusicTool, MusicArgs;
        public double Scale = 1;
        public string Problem;

        public static Args Parse(string[] a)
        {
            var r = new Args();
            for (int i = 0; i < a.Length; i++)
            {
                string k = a[i];
                Func<string> next = () =>
                {
                    if (i + 1 >= a.Length) { r.Problem = k + " needs a value"; return null; }
                    return a[++i];
                };
                switch (k.ToLowerInvariant())
                {
                    case "--silent": case "/s": case "/silent": r.Silent = true; break;
                    case "--test": r.Test = true; break;
                    case "--pack": r.Pack = true; break;
                    case "--list": r.List = true; break;
                    case "--selftest": r.SelfTest = true; break;
                    case "--uninstall": r.Uninstall = true; break;
                    case "--remove-saves": r.RemoveSaves = true; break;
                    case "--no-hd-loading": r.NoHd = true; break;
                    case "--no-desktop": r.NoDesktop = true; break;
                    case "--no-start-menu": r.NoStartMenu = true; break;
                    case "--launch": r.Launch = true; break;
                    case "--pad": r.Pad = true; break;
                    case "--quiet": r.Quiet = true; break;
                    case "--iso": r.Iso = next(); break;
                    case "--dir": r.Dir = next(); break;
                    case "--log": r.Log = next(); break;
                    case "--home": r.Home = next(); break;
                    case "--screenshot": r.Screenshot = next(); break;
                    case "--only": r.Only = next(); break;
                    case "--package": r.Package = next(); break;
                    case "--hd-loading": r.HdLoading = next(); break;
                    case "--setup-files": r.SetupFiles = next(); break;
                    case "--out": r.Out = next(); break;
                    case "--version": r.Version = next(); break;
                    case "--publisher": r.Publisher = next(); break;
                    case "--music-tool": r.MusicTool = next(); break;
                    case "--music-args": r.MusicArgs = next(); break;
                    case "--scale":
                        {
                            string v = next();
                            double s;
                            if (v != null && double.TryParse(v, NumberStyles.Float, CultureInfo.InvariantCulture, out s) && s >= 0.5 && s <= 4) r.Scale = s;
                            else r.Problem = "--scale takes a number from 0.5 to 4";
                            break;
                        }
                    default: r.Problem = "unknown argument " + k; break;
                }
                if (r.Problem != null) break;
            }
            if (r.Problem == null && r.Silent && !r.Uninstall && !IsUninstallerName() && string.IsNullOrEmpty(r.Iso)) r.Problem = "--silent needs --iso <file or folder>";
            if (r.Problem == null && r.Pack && (string.IsNullOrEmpty(r.Package) || string.IsNullOrEmpty(r.Out))) r.Problem = "--pack needs --package <folder> and --out <file>";
            return r;
        }

        public static bool IsUninstallerName()
        {
            return string.Equals(Path.GetFileName(Env.SelfPath), "Uninstall.exe", StringComparison.OrdinalIgnoreCase);
        }
    }

    public static class Program
    {
        [DllImport("kernel32.dll")]
        static extern bool AttachConsole(int pid);
        [DllImport("kernel32.dll")]
        static extern IntPtr GetStdHandle(int which);

        public static string Version
        {
            get
            {
                var v = typeof(Program).Assembly.GetName().Version;
                return v.Major + "." + v.Minor + "." + v.Build;
            }
        }

        /// <summary>A program without a console prints only where its output is redirected, or to the console it was started from.</summary>
        static void Console_()
        {
            try
            {
                var h = GetStdHandle(-11);
                if (h == IntPtr.Zero || h == new IntPtr(-1)) { AttachConsole(-1); Console.SetOut(new StreamWriter(Console.OpenStandardOutput()) { AutoFlush = true }); }
            }
            catch { }
        }

        [STAThread]
        public static int Main(string[] argv)
        {
            var a = Args.Parse(argv);
            if (a.Problem != null)
            {
                Console_();
                Console.Error.WriteLine("Setup: " + a.Problem);
                return Exit.BadArguments;
            }
            if (!string.IsNullOrEmpty(a.Home)) Env.Home = Path.GetFullPath(a.Home);
            bool uninstaller = a.Uninstall || Args.IsUninstallerName();
            string mode = a.Pack ? "pack" : a.Test ? "test" : a.Screenshot != null ? "screenshot" : a.SelfTest ? "selftest" : uninstaller ? (a.Silent ? "uninstall silent" : "uninstall") : a.Silent ? "silent" : "window";
            if (a.Pack || a.Test || a.List || a.Screenshot != null) Console_();
            if (a.List) return List();
            if (!a.Pack && !a.Test && a.Screenshot == null)
                Log.Open(Util.Full(a.Log) ?? Env.DefaultLog, "Setup " + Version + " " + mode + (Env.IsTest ? " (test home " + Env.Home + ")" : "") + "; " + Util.WindowsVersion() + "; " + string.Join(" ", argv.Select(Util.Quote)));
            Env.RemoveStaleTemps();
            try
            {
                if (a.Pack)
                {
                    var input = new Packer.Input { Package = a.Package, HdLoading = a.HdLoading, SetupFiles = a.SetupFiles, Out = a.Out };
                    if (a.Version != null) input.Version = a.Version;
                    if (a.Publisher != null) input.Publisher = a.Publisher;
                    if (a.MusicTool != null) input.MusicTool = a.MusicTool;
                    if (a.MusicArgs != null) input.MusicArgs = a.MusicArgs;
                    return Packer.Run(input, Console.Out);
                }
                if (a.Test) return Tests.Run(a.Iso);
                if (a.Screenshot != null) return Shots.Run(a.Screenshot, a.Scale, a.Iso, a.Only);
                if (uninstaller) return a.Silent ? SilentUninstall(a) : Window(a, null, true);
                Payload payload = null;
                string damaged = null;
                try
                {
                    payload = Payload.Open(Env.SelfPath);
                    if (payload == null) damaged = "The installer's data is damaged. Download it again.";
                }
                catch (DamagedException e) { Log.Error("the payload: " + e.Message); damaged = "The installer's data is damaged. Download it again."; }
                catch (Exception e) { Log.Error("the payload cannot be read: " + e.Message); damaged = "The installer's data is damaged. Download it again."; }
                if (payload == null) Log.Line("no payload in " + Env.SelfPath);
                else Log.Line("payload " + payload.Version + " built " + payload.Built + ": " + payload.Files.Count + " files, parts " + string.Join(", ", payload.Files.GroupBy(f => f.Part).Select(g => g.Key + " " + Util.Mb(g.Sum(f => f.Raw)))));
                if (a.Silent) return Silent(a, payload);
                return Window(a, payload, false, damaged);
            }
            catch (Exception e)
            {
                Log.Error(e.ToString());
                if (a.Pack || a.Test || a.Screenshot != null) Console.Error.WriteLine(e);
                return Exit.Other;
            }
            finally { Env.RemoveTemp(); }
        }

        /// <summary>--list: the payload's files, one per line: part, size, path.</summary>
        static int List()
        {
            try
            {
                var p = Payload.Open(Env.SelfPath);
                if (p == null) { Console.WriteLine("no payload"); return Exit.Damaged; }
                Console.WriteLine("# " + Env.Product + " " + p.Version + ", built " + p.Built + ", music " + (p.MusicTool ?? "none") + " " + (p.MusicArgs ?? ""));
                foreach (var f in p.Files) Console.WriteLine(f.Part + " " + f.Raw.ToString(CultureInfo.InvariantCulture) + " " + f.Path);
                return Exit.Done;
            }
            catch (Exception e) { Console.WriteLine("damaged: " + e.Message); return Exit.Damaged; }
        }

        static int Silent(Args a, Payload payload)
        {
            if (payload == null) { Log.Error("there is nothing to install: the payload is missing or damaged"); return Exit.Damaged; }
            var disc = Disc.Check(a.Iso, false);
            Log.Line("disc " + disc.Path + ": " + (disc.Ok ? "accepted" : disc.Verdict + " (" + disc.Message + ")") + ", id " + (disc.Id.Length > 0 ? disc.Id : "-") + ", BOOT.BIN " + disc.BootSize + " bytes fnv1a64 " + disc.BootHash.ToString("x16"));
            if (!disc.Ok) return Exit.DiscRefused;
            var options = new InstallOptions
            {
                Dir = string.IsNullOrEmpty(a.Dir) ? Env.DefaultDir : a.Dir,
                Disc = disc.Path,
                HdLoading = !a.NoHd && payload.HasPart("hd_loading"),
                Desktop = !a.NoDesktop, StartMenu = !a.NoStartMenu, Launch = a.Launch,
            };
            var installer = new Installer(payload, options);
            var result = installer.Run();
            Log.Line("exit " + result.Code + (result.Message.Length > 0 ? " (" + result.Message + ")" : "") + (result.Warnings.Count > 0 ? "; " + string.Join(" ", result.Warnings) : ""));
            if (result.Code == Exit.Done && a.Launch)
            {
                try { System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(Path.Combine(result.Dir, "ChiliConCarnageLauncher.exe")) { UseShellExecute = false, WorkingDirectory = result.Dir }); }
                catch (Exception e) { Log.Error("could not start the launcher: " + e.Message); }
            }
            return result.Code;
        }

        static int SilentUninstall(Args a)
        {
            string dir = Path.GetDirectoryName(Env.SelfPath);
            if (!string.IsNullOrEmpty(a.Dir)) dir = Util.Full(a.Dir);
            var r = Uninstaller.Run(dir, a.RemoveSaves);
            Log.Line("exit " + r.Code + (r.Message.Length > 0 ? " (" + r.Message + ")" : ""));
            if (r.Code == Exit.Done) Uninstaller.RemoveSelfLater(dir);
            return r.Code;
        }

        static int Window(Args a, Payload payload, bool uninstaller, string damaged = null)
        {
            var app = new Application { ShutdownMode = ShutdownMode.OnMainWindowClose };
            Faces.Load();
            Log.Line("fonts: " + Faces.Loaded);
            var m = new SetupModel { Uninstaller = uninstaller, Version = payload != null && payload.Version.Length > 0 ? payload.Version : Version, Damaged = damaged };
            if (payload != null)
            {
                m.ProgramBytes = payload.PartBytes("program");
                m.HdBytes = payload.PartBytes("hd_loading");
                m.HasHd = payload.HasPart("hd_loading");
                m.HdLoading = m.HasHd;
            }
            if (uninstaller)
            {
                m.Page = Page.Uninstall;
                m.Dir = string.IsNullOrEmpty(a.Dir) ? Path.GetDirectoryName(Env.SelfPath) : Util.Full(a.Dir);
                if (InstallRecord.Load(m.Dir) == null) { m.Removing = 3; m.RemoveMessage = "There is no installation here."; }
            }
            else m.Dir = SetupWindow.DefaultDir();
            var window = new SetupWindow(m, payload) { UninstallDir = uninstaller ? m.Dir : null, ExitCode = Exit.Cancelled };
            Log.Line("window " + window.View.Width + "x" + window.View.Height + " units");
            bool quiet = a.Quiet || Environment.GetEnvironmentVariable("CHILI_SETUP_QUIET") == "1" || a.SelfTest;
            if (quiet) { window.ShowActivated = false; window.Quiet = true; }
            window.Loaded += (s, e) =>
            {
                try { Log.Line("monitor scale " + (VisualTreeHelper_Dpi(window) * 100).ToString("0", CultureInfo.InvariantCulture) + " %"); } catch { }
            };
            app.DispatcherUnhandledException += (s, e) =>
            {
                Log.Error("in the window: " + e.Exception);
                e.Handled = true;
            };
            if (a.SelfTest) SelfTest.Attach(window, a);
            app.Run(window);
            window.Music.StopNow(0);
            return a.SelfTest ? SelfTest.Failures : window.ExitCode;
        }

        static double VisualTreeHelper_Dpi(Window w) { return System.Windows.Media.VisualTreeHelper.GetDpi(w).DpiScaleX; }
    }
}
