// The install: the folder rules, the two-phase write (everything into a hidden stage folder first, then
// moved into place), the settings files, the shortcuts, the Apps entry, install.manifest; and the uninstall.
// A player's saves are never read, listed, hashed, copied or deleted, except by the uninstaller when the
// player asked for it twice.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.ComTypes;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using Microsoft.Win32;

namespace ChiliSetup
{
    public static class Exit
    {
        public const int Done = 0, Cancelled = 1, BadArguments = 2, DiscRefused = 3, FolderRefused = 4, NoSpace = 5, Damaged = 6, Running = 7, WriteFailed = 8, Other = 9;
    }

    public sealed class InstallOptions
    {
        public string Dir, Disc;
        public bool HdLoading = true, Desktop = true, StartMenu = true, Launch = true;
    }

    public enum FolderVerdict { Ok, Update, Invalid, Drive, NotEmpty, NotWritable, NoSpace, Running }

    public sealed class FolderCheck
    {
        public FolderVerdict Verdict;
        public string Dir = "", Message = "";
        public long Free = -1, Need;
        public bool Ok { get { return Verdict == FolderVerdict.Ok || Verdict == FolderVerdict.Update; } }
        public int ExitCode
        {
            get
            {
                switch (Verdict)
                {
                    case FolderVerdict.NoSpace: return Exit.NoSpace;
                    case FolderVerdict.Running: return Exit.Running;
                    case FolderVerdict.Ok: case FolderVerdict.Update: return Exit.Done;
                    default: return Exit.FolderRefused;
                }
            }
        }
    }

    public static class Folder
    {
        public const string TextNotEmpty = "This folder is not empty; choose an empty one.";
        public const string TextNotWritable = "This folder cannot be written.";
        public const string TextNoSpace = "Not enough free space.";
        public const string TextDrive = "Choose a folder, not a drive.";
        public const string TextRunning = "Close the game and the launcher first.";
        public const string TextUpdate = "An earlier installation is here; saves, settings and mods are kept.";

        /// <summary>
        /// A folder that holds an earlier installation: the game program, Setup's record of one, or what an
        /// uninstall keeps (the settings, the saves), so that installing again where it was is an update.
        /// </summary>
        public static bool HoldsInstall(string dir)
        {
            if (string.IsNullOrEmpty(dir) || !Directory.Exists(dir)) return false;
            foreach (var name in new[] { "ChiliConCarnage.exe", "install.manifest", "ChiliNative.ini", "launcher.ini" })
                if (File.Exists(Path.Combine(dir, name))) return true;
            return Directory.Exists(Path.Combine(dir, "saves", "PSP"));
        }

        /// <summary>
        /// The rules of page 3. `probe` writes and deletes a file to see that the folder can be written (making
        /// the folder for that and removing it again); `running` looks for the game or the launcher in it.
        /// </summary>
        public static FolderCheck Check(string given, long need, bool probe = true, bool running = true)
        {
            var c = new FolderCheck { Need = need };
            string dir = Util.Full(given);
            if (dir == null) { c.Verdict = FolderVerdict.Invalid; c.Message = TextNotWritable; return c; }
            dir = dir.TrimEnd('\\');
            c.Dir = dir;
            string root = Path.GetPathRoot(dir);
            if (string.IsNullOrEmpty(root) || string.Equals(root.TrimEnd('\\'), dir, StringComparison.OrdinalIgnoreCase) || dir.Length <= 3)
            {
                c.Verdict = FolderVerdict.Drive; c.Message = TextDrive; return c;
            }
            if (File.Exists(dir) || Util.SameOrInside(dir, Path.Combine(Path.GetTempPath(), "ChiliConCarnagePC-Setup-" + Process.GetCurrentProcess().Id)))
            {
                c.Verdict = FolderVerdict.NotWritable; c.Message = TextNotWritable; return c;
            }
            bool earlier = HoldsInstall(dir);
            if (Directory.Exists(dir) && !earlier)
            {
                bool any;
                try { any = Directory.EnumerateFileSystemEntries(dir).Any(); }
                catch { c.Verdict = FolderVerdict.NotWritable; c.Message = TextNotWritable; return c; }
                if (any) { c.Verdict = FolderVerdict.NotEmpty; c.Message = TextNotEmpty; return c; }
            }
            if (running && earlier && Util.RunningFrom(dir) != null) { c.Verdict = FolderVerdict.Running; c.Message = TextRunning; return c; }
            if (probe && !Writable(dir)) { c.Verdict = FolderVerdict.NotWritable; c.Message = TextNotWritable; return c; }
            c.Free = Util.FreeSpace(dir);
            if (c.Free >= 0 && c.Free < need) { c.Verdict = FolderVerdict.NoSpace; c.Message = TextNoSpace; return c; }
            c.Verdict = earlier ? FolderVerdict.Update : FolderVerdict.Ok;
            c.Message = earlier ? TextUpdate : "";
            return c;
        }

        static bool Writable(string dir)
        {
            // The folders made only for the probe are removed again, deepest first.
            var made = new List<string>();
            string p = dir;
            while (!string.IsNullOrEmpty(p) && !Directory.Exists(p)) { made.Add(p); p = Path.GetDirectoryName(p); }
            try
            {
                Directory.CreateDirectory(dir);
                string probe = Path.Combine(dir, ".chili_setup_probe");
                File.WriteAllText(probe, "probe");
                File.Delete(probe);
                return true;
            }
            catch { return false; }
            finally
            {
                foreach (var m in made) try { if (Directory.Exists(m) && !Directory.EnumerateFileSystemEntries(m).Any()) Directory.Delete(m); } catch { }
            }
        }
    }

    /// <summary>The [Disc] Path line of ChiliNative.ini, written as package.ps1 -Disc writes it.</summary>
    public static class IniEdit
    {
        static readonly Regex example = new Regex(@"(?ms)(^\[Disc\][^\[]*?^); ?Path=[^\r\n]*");

        /// <summary>The text with `Path=<disc>` over the commented example of [Disc] (or added), line ends kept.</summary>
        public static string SetDiscPath(string text, string disc)
        {
            string newline = text.Contains("\r\n") ? "\r\n" : "\n";
            string line = "Path=" + disc;
            if (example.IsMatch(text)) return example.Replace(text, m => m.Groups[1].Value + line, 1);
            var section = new Regex(@"(?m)^\[Disc\][^\r\n]*(\r?\n)");
            if (section.IsMatch(text)) return section.Replace(text, m => m.Value + line + newline, 1);
            if (text.Length > 0 && !text.EndsWith("\n")) text += newline;
            return text + newline + "[Disc]" + newline + line + newline;
        }

        /// <summary>The disc as the game reads it: relative when it lies inside the game's folder.</summary>
        public static string DiscValue(string disc, string dir)
        {
            string full = Util.Full(disc) ?? disc;
            if (Util.SameOrInside(full, dir) && full.Length > dir.TrimEnd('\\').Length + 1) return full.Substring(dir.TrimEnd('\\').Length + 1);
            return full;
        }
    }

    /// <summary>A Windows shortcut (.lnk), through the shell's own IShellLink.</summary>
    public static class Shortcut
    {
        [ComImport, Guid("00021401-0000-0000-C000-000000000046")]
        class ShellLink { }

        [ComImport, InterfaceType(ComInterfaceType.InterfaceIsIUnknown), Guid("000214F9-0000-0000-C000-000000000046")]
        interface IShellLinkW
        {
            void GetPath([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder file, int max, IntPtr data, uint flags);
            void GetIDList(out IntPtr list);
            void SetIDList(IntPtr list);
            void GetDescription([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder name, int max);
            void SetDescription([MarshalAs(UnmanagedType.LPWStr)] string name);
            void GetWorkingDirectory([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder dir, int max);
            void SetWorkingDirectory([MarshalAs(UnmanagedType.LPWStr)] string dir);
            void GetArguments([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder args, int max);
            void SetArguments([MarshalAs(UnmanagedType.LPWStr)] string args);
            void GetHotkey(out short key);
            void SetHotkey(short key);
            void GetShowCmd(out int show);
            void SetShowCmd(int show);
            void GetIconLocation([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder path, int max, out int index);
            void SetIconLocation([MarshalAs(UnmanagedType.LPWStr)] string path, int index);
            void SetRelativePath([MarshalAs(UnmanagedType.LPWStr)] string path, uint reserved);
            void Resolve(IntPtr hwnd, uint flags);
            void SetPath([MarshalAs(UnmanagedType.LPWStr)] string file);
        }

        public static void Create(string file, string target, string arguments, string workingDir, string icon, string description)
        {
            Exception failed = null;
            // The shell's objects want a single-threaded apartment: a thread of its own, whoever calls.
            var t = new Thread(() =>
            {
                try
                {
                    var link = (IShellLinkW)new ShellLink();
                    link.SetPath(target);
                    if (!string.IsNullOrEmpty(arguments)) link.SetArguments(arguments);
                    link.SetWorkingDirectory(workingDir);
                    link.SetIconLocation(icon, 0);
                    link.SetDescription(description);
                    Directory.CreateDirectory(Path.GetDirectoryName(file));
                    ((IPersistFile)link).Save(file, true);
                    Marshal.ReleaseComObject(link);
                }
                catch (Exception e) { failed = e; }
            });
            t.SetApartmentState(ApartmentState.STA);
            t.Start();
            t.Join();
            if (failed != null) throw failed;
            if (!File.Exists(file)) throw new IOException("the shortcut was not written");
        }
    }

    /// <summary>install.manifest: what Setup wrote, so that an update and the uninstaller know.</summary>
    public sealed class InstallRecord
    {
        public string Version = "", Installed = "", Disc = "", UninstallKey = "", SettingsKey = "";
        public bool Test, HdLoading;
        public readonly List<PayloadFile> Files = new List<PayloadFile>();     // Part is "program", "hd_loading" or "setup-made"
        public readonly List<string> Shortcuts = new List<string>();

        public static InstallRecord Load(string dir)
        {
            string file = Path.Combine(dir, "install.manifest");
            if (!File.Exists(file)) return null;
            try
            {
                var root = Json.Read(File.ReadAllText(file, Encoding.UTF8));
                var r = new InstallRecord
                {
                    Version = Json.S(root, "version", ""), Installed = Json.S(root, "installed", ""), Disc = Json.S(root, "disc", ""),
                    UninstallKey = Json.S(root, "uninstallKey", ""), SettingsKey = Json.S(root, "settingsKey", ""),
                    Test = Json.B(root, "test"), HdLoading = Json.B(root, "hdLoading"),
                };
                foreach (var f in Json.Arr(root, "files") ?? new List<object>())
                    r.Files.Add(new PayloadFile { Path = Json.S(f, "path", ""), Part = Json.S(f, "part", ""), Raw = Json.L(f, "size"), Sha256 = Json.S(f, "sha256", "") });
                foreach (var s in Json.Arr(root, "shortcuts") ?? new List<object>()) if (s is string) r.Shortcuts.Add((string)s);
                r.Files.RemoveAll(f => f.Path.Length == 0 || f.Path.Contains("..") || Path.IsPathRooted(f.Path));
                return r;
            }
            catch (Exception e) { Log.Line("install.manifest cannot be read: " + e.Message); return null; }
        }

        public void Save(string dir)
        {
            var root = new Dictionary<string, object>
            {
                { "product", Env.Product }, { "version", Version }, { "installed", Installed }, { "disc", Disc },
                { "hdLoading", HdLoading }, { "test", Test }, { "uninstallKey", UninstallKey }, { "settingsKey", SettingsKey },
                { "shortcuts", Shortcuts.Cast<object>().ToList() },
                { "kept", new List<object> { "saves", "ChiliNative.ini", "launcher.ini", "logs", "art", "textures", "texture_dump", "mods (the player's own)" } },
                { "files", Files.Select(f => (object)new Dictionary<string, object> { { "path", f.Path }, { "part", f.Part }, { "size", f.Raw }, { "sha256", f.Sha256 } }).ToList() },
            };
            File.WriteAllText(Path.Combine(dir, "install.manifest"), Json.Write(root), new UTF8Encoding(false));
        }
    }

    public sealed class InstallResult
    {
        public int Code;
        public string Message = "";
        public readonly List<string> Warnings = new List<string>();
        public bool Update;
        public string Dir = "";
    }

    /// <summary>One install, run on a worker thread (the window) or on the main thread (--silent).</summary>
    public sealed class Installer
    {
        readonly Payload payload;
        readonly InstallOptions options;
        public Action<long, long, string> Progress = delegate { };
        public volatile bool Cancel;
        /// <summary>Tests: stop with this message after the stage is written, as a full disk would.</summary>
        public string FailAfterStage;

        public Installer(Payload p, InstallOptions o) { payload = p; options = o; }

        public long Need { get { return payload.PartBytes("program") + (options.HdLoading ? payload.PartBytes("hd_loading") : 0) + (50L << 20); } }

        static int CommitOrder(string path)
        {
            string p = path.ToLowerInvariant();
            if (p == "chiliconcarnage.exe") return 0;
            if (!p.Contains("/") && p.EndsWith(".dll")) return 1;
            if (p == "chiliconcarnagelauncher.exe") return 2;
            return 3;
        }

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern bool MoveFileEx(string from, string to, int flags);

        public InstallResult Run()
        {
            var result = new InstallResult();
            var clock = Stopwatch.StartNew();
            var folder = Folder.Check(options.Dir, Need);
            result.Dir = folder.Dir;
            if (!folder.Ok) { result.Code = folder.ExitCode; result.Message = folder.Message; Log.Error("folder " + options.Dir + ": " + folder.Message); return result; }
            string dir = folder.Dir;
            result.Update = folder.Verdict == FolderVerdict.Update;
            Log.Line("folder " + dir + (result.Update ? " (an earlier installation: update)" : "") + ", " + Util.Free(folder.Free) + " free, " + Util.Mb(folder.Need) + " needed");
            Log.Line("options: hd loading " + options.HdLoading + ", desktop " + options.Desktop + ", start menu " + options.StartMenu + ", start when done " + options.Launch);
            bool created = !Directory.Exists(dir);
            string stage = Path.Combine(dir, ".setup-stage");
            var earlier = InstallRecord.Load(dir);
            try
            {
                Directory.CreateDirectory(dir);
                if (Directory.Exists(stage)) Directory.Delete(stage, true);
                var stageInfo = Directory.CreateDirectory(stage);
                stageInfo.Attributes |= FileAttributes.Hidden;
            }
            catch (Exception e)
            {
                Log.Error("the folder cannot be made: " + e.Message);
                result.Code = Exit.FolderRefused; result.Message = Folder.TextNotWritable;
                return result;
            }

            var files = payload.Files.Where(f => f.Part == "program" || (f.Part == "hd_loading" && options.HdLoading))
                .OrderBy(f => CommitOrder(f.Path)).ThenBy(f => f.Path, StringComparer.OrdinalIgnoreCase).ToList();
            long total = files.Sum(f => f.Raw), done = 0;
            var staged = new List<KeyValuePair<string, PayloadFile>>();
            try
            {
                // 1. Everything into the stage.
                foreach (var f in files)
                {
                    if (Cancel) return RollBack(result, dir, stage, created, "cancelled while writing");
                    Progress(done, total, f.Path);
                    var bytes = payload.Read(f);
                    string target = Path.Combine(stage, f.Path.Replace('/', '\\'));
                    Directory.CreateDirectory(Path.GetDirectoryName(target));
                    using (var o = new FileStream(target, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 20)) o.Write(bytes, 0, bytes.Length);
                    staged.Add(new KeyValuePair<string, PayloadFile>(f.Path, f));
                    done += f.Raw;
                    Log.Line(string.Format(CultureInfo.InvariantCulture, "staged {0} ({1:N0} bytes)", f.Path, f.Raw));
                }
                payload.Forget();
                Progress(done, total, "");
                // 2. What Setup makes itself: the uninstaller (this program without its payload), the settings
                //    files when the folder has none, the shortcut to the settings.
                var made = new List<PayloadFile>();
                string uninstall = Path.Combine(stage, "Uninstall.exe");
                CopyStub(uninstall);
                made.Add(new PayloadFile { Path = "Uninstall.exe", Part = "setup-made", Raw = new FileInfo(uninstall).Length, Sha256 = Util.Sha256File(uninstall) });
                if (FailAfterStage != null) throw new IOException(FailAfterStage);
                if (Cancel) return RollBack(result, dir, stage, created, "cancelled before the commit");

                // 3. Commit: a move per file, the game program and its DLLs first.
                Progress(total, total, "");
                foreach (var pair in staged.Concat(made.Select(m => new KeyValuePair<string, PayloadFile>(m.Path, m))))
                {
                    string from = Path.Combine(stage, pair.Key.Replace('/', '\\')), to = Path.Combine(dir, pair.Key.Replace('/', '\\'));
                    Directory.CreateDirectory(Path.GetDirectoryName(to));
                    if (!MoveFileEx(from, to, 0x1 /* REPLACE_EXISTING */ | 0x2 /* COPY_ALLOWED */))
                    {
                        string why = new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error()).Message;
                        Log.Error("could not write " + to + ": " + why);
                        result.Code = Exit.WriteFailed;
                        result.Message = "Could not write " + Path.GetFileName(to) + ".";
                        TryDelete(stage);
                        return result;
                    }
                }
                Log.Line("committed " + (staged.Count + made.Count) + " files");

                // 4. The record. Files of the earlier record that this one does not name are removed (a renamed
                //    document); the earlier pack's pictures stay when the pack was not chosen this time, and stay
                //    in the record so that the uninstaller still knows them.
                var record = new InstallRecord
                {
                    Version = payload.Version, Installed = DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss", CultureInfo.InvariantCulture),
                    Disc = options.Disc ?? "", Test = Env.IsTest, HdLoading = options.HdLoading,
                    UninstallKey = Env.UninstallKeyName, SettingsKey = Env.SettingsKey,
                };
                record.Files.AddRange(staged.Select(p => new PayloadFile { Path = p.Value.Path, Part = p.Value.Part, Raw = p.Value.Raw, Sha256 = p.Value.Sha256 }));
                record.Files.AddRange(made);
                var named = new HashSet<string>(record.Files.Select(f => f.Path), StringComparer.OrdinalIgnoreCase);
                if (earlier != null)
                {
                    foreach (var old in earlier.Files)
                    {
                        if (named.Contains(old.Path) || old.Path.Equals("Chili Con Carnage settings.lnk", StringComparison.OrdinalIgnoreCase)) continue;
                        if (old.Part == "hd_loading" && !options.HdLoading)
                        {
                            if (File.Exists(Path.Combine(dir, old.Path.Replace('/', '\\')))) { record.Files.Add(old); named.Add(old.Path); }
                            continue;
                        }
                        string gone = Path.Combine(dir, old.Path.Replace('/', '\\'));
                        try { if (File.Exists(gone)) { File.Delete(gone); Log.Line("removed " + old.Path + " (not in this version)"); } }
                        catch (Exception e) { Log.Line("could not remove " + old.Path + ": " + e.Message); }
                    }
                }
                TryDelete(stage);

                // 5. Settings: written once; a file the folder already has is the player's and is kept. Only
                //    when it names no disc at all is the chosen one written into it.
                string ini = Path.Combine(dir, "ChiliNative.ini");
                string discValue = string.IsNullOrEmpty(options.Disc) ? null : IniEdit.DiscValue(options.Disc, dir);
                var template = payload.Files.FirstOrDefault(f => f.Path == "template/ChiliNative.ini");
                if (!File.Exists(ini))
                {
                    string text = template != null ? Encoding.UTF8.GetString(payload.Read(template)) : "";
                    if (text.Length > 0 && text[0] == '﻿') text = text.Substring(1);
                    if (discValue != null) text = IniEdit.SetDiscPath(text, discValue);
                    File.WriteAllText(ini, text, new UTF8Encoding(false));
                    Log.Line("wrote ChiliNative.ini" + (discValue != null ? " with [Disc] Path=" + discValue : ""));
                }
                else
                {
                    string text = File.ReadAllText(ini);
                    if (discValue != null && !Packer.LiveDiscPath(text))
                    {
                        File.WriteAllText(ini, IniEdit.SetDiscPath(text, discValue), new UTF8Encoding(false));
                        Log.Line("ChiliNative.ini kept; it named no disc, [Disc] Path=" + discValue + " added");
                    }
                    else Log.Line("ChiliNative.ini kept as it is");
                }
                string launcherIni = Path.Combine(dir, "launcher.ini");
                var launcherTemplate = payload.Files.FirstOrDefault(f => f.Path == "template/launcher.ini");
                if (!File.Exists(launcherIni) && launcherTemplate != null)
                {
                    File.WriteAllBytes(launcherIni, payload.Read(launcherTemplate));
                    Log.Line("wrote launcher.ini");
                }
                else Log.Line("launcher.ini kept as it is");
                payload.Forget();

                // 6. Shortcuts: the settings one in the folder, the Start menu's, the desktop's.
                string launcher = Path.Combine(dir, "ChiliConCarnageLauncher.exe");
                var links = new List<KeyValuePair<string, string>>();          // file, arguments
                links.Add(new KeyValuePair<string, string>(Path.Combine(dir, "Chili Con Carnage settings.lnk"), "--settings"));
                if (options.StartMenu)
                {
                    links.Add(new KeyValuePair<string, string>(Path.Combine(Env.StartMenuFolder, "Chili Con Carnage.lnk"), ""));
                    links.Add(new KeyValuePair<string, string>(Path.Combine(Env.StartMenuFolder, "Chili Con Carnage settings.lnk"), "--settings"));
                }
                if (options.Desktop) links.Add(new KeyValuePair<string, string>(Path.Combine(Env.Desktop, "Chili Con Carnage.lnk"), ""));
                bool linkFailed = false;
                foreach (var link in links)
                {
                    try
                    {
                        Shortcut.Create(link.Key, launcher, link.Value, dir, launcher, link.Value.Length > 0 ? "Chili Con Carnage: settings" : "Chili Con Carnage");
                        Log.Line("shortcut " + link.Key);
                        if (Util.SameOrInside(link.Key, dir))
                        {
                            string rel = link.Key.Substring(dir.Length + 1).Replace('\\', '/');
                            record.Files.Add(new PayloadFile { Path = rel, Part = "setup-made", Raw = new FileInfo(link.Key).Length, Sha256 = Util.Sha256File(link.Key) });
                        }
                        else record.Shortcuts.Add(link.Key);
                    }
                    catch (Exception e) { linkFailed = true; Log.Error("shortcut " + link.Key + ": " + e.Message); }
                }
                if (linkFailed) result.Warnings.Add("Could not write the shortcut.");
                // Shortcuts an earlier install made that were not wanted this time.
                if (earlier != null)
                    foreach (var old in earlier.Shortcuts)
                        if (!record.Shortcuts.Contains(old, StringComparer.OrdinalIgnoreCase))
                        {
                            try { if (File.Exists(old)) { File.Delete(old); Log.Line("removed the earlier shortcut " + old); } } catch { }
                            RemoveShortcutFolder(Path.GetDirectoryName(old));
                        }
                record.Save(dir);
                Log.Line("wrote install.manifest (" + record.Files.Count + " files)");

                // 7. Windows' list of installed apps, and what Setup remembers for its next run.
                try { Register(dir, record); }
                catch (Exception e) { Log.Error("the Apps entry: " + e.Message); result.Warnings.Add("Could not register the program."); }
                Remembered.Set("LastFolder", dir);
                if (!string.IsNullOrEmpty(options.Disc)) Remembered.Set("LastIso", Util.Full(options.Disc) ?? options.Disc);

                Log.Line(string.Format(CultureInfo.InvariantCulture, "installed in {0:0.0} s", clock.Elapsed.TotalSeconds));
                result.Code = Exit.Done;
                CopyLog(dir);
                return result;
            }
            catch (DamagedException e)
            {
                Log.Error("the installer's data is damaged: " + e.Message);
                RollBack(result, dir, stage, created, "damaged");
                result.Code = Exit.Damaged; result.Message = "The installer's data is damaged. Download it again.";
                return result;
            }
            catch (IOException e) when (IsDiskFull(e))
            {
                Log.Error("disk full: " + e.Message);
                RollBack(result, dir, stage, created, "disk full");
                result.Code = Exit.NoSpace; result.Message = "Not enough free space on " + Path.GetPathRoot(dir).TrimEnd('\\') + ".";
                return result;
            }
            catch (Exception e)
            {
                Log.Error(e.GetType().Name + ": " + e.Message);
                RollBack(result, dir, stage, created, "failed");
                result.Code = e is UnauthorizedAccessException || e is IOException ? Exit.WriteFailed : Exit.Other;
                result.Message = e is UnauthorizedAccessException || e is IOException ? Folder.TextNotWritable : "The installation failed.";
                return result;
            }
        }

        static bool IsDiskFull(IOException e)
        {
            int code = Marshal.GetHRForException(e) & 0xFFFF;
            return code == 112 || code == 39;          // ERROR_DISK_FULL, ERROR_HANDLE_DISK_FULL
        }

        InstallResult RollBack(InstallResult result, string dir, string stage, bool created, string why)
        {
            TryDelete(stage);
            if (created) TryDelete(dir);
            Log.Line("rolled back (" + why + "): the stage was removed" + (created ? ", and the folder Setup had made" : "; the earlier installation is as it was"));
            result.Code = Exit.Cancelled;
            result.Message = "Nothing was installed.";
            return result;
        }

        static void TryDelete(string dir)
        {
            try { if (Directory.Exists(dir)) Directory.Delete(dir, true); } catch (Exception e) { Log.Line("could not remove " + dir + ": " + e.Message); }
        }
        /// <summary>
        /// The folder of a removed shortcut, when it is Setup's own Start menu folder and now empty. Never the
        /// desktop or any other folder of the player's, empty or not.
        /// </summary>
        public static void RemoveShortcutFolder(string dir)
        {
            if (string.IsNullOrEmpty(dir) || !string.Equals(Path.GetFileName(dir.TrimEnd('\\')), Env.FolderName, StringComparison.OrdinalIgnoreCase)) return;
            RemoveIfEmpty(dir);
        }
        public static void RemoveIfEmpty(string dir)
        {
            try { if (Directory.Exists(dir) && !Directory.EnumerateFileSystemEntries(dir).Any()) Directory.Delete(dir); } catch { }
        }

        /// <summary>This program without its payload: the uninstaller.</summary>
        static void CopyStub(string target)
        {
            string self = Env.SelfPath;
            long length = Payload.StubLength(self);
            using (var s = new FileStream(self, FileMode.Open, FileAccess.Read, FileShare.Read))
            using (var o = new FileStream(target, FileMode.Create, FileAccess.Write))
            {
                var buffer = new byte[1 << 20];
                long left = length;
                while (left > 0) { int r = s.Read(buffer, 0, (int)Math.Min(buffer.Length, left)); if (r <= 0) break; o.Write(buffer, 0, r); left -= r; }
            }
        }

        void Register(string dir, InstallRecord record)
        {
            string version = payload.Version;
            try
            {
                var info = FileVersionInfo.GetVersionInfo(Path.Combine(dir, "ChiliConCarnage.exe"));
                if (!string.IsNullOrEmpty(info.ProductVersion)) version = info.ProductVersion;
            }
            catch { }
            long bytes = 0;
            foreach (var f in record.Files) bytes += f.Raw;
            using (var k = Registry.CurrentUser.CreateSubKey(Env.UninstallKeyRoot + Env.UninstallKeyName))
            {
                k.SetValue("DisplayName", Env.IsTest ? Env.Product + " (test)" : Env.Product);
                k.SetValue("DisplayVersion", version);
                k.SetValue("Publisher", string.IsNullOrEmpty(payload.Publisher) ? "Chili Con Carnage PC project" : payload.Publisher);
                k.SetValue("InstallLocation", dir);
                k.SetValue("DisplayIcon", Path.Combine(dir, "ChiliConCarnageLauncher.exe") + ",0");
                k.SetValue("UninstallString", "\"" + Path.Combine(dir, "Uninstall.exe") + "\"");
                k.SetValue("QuietUninstallString", "\"" + Path.Combine(dir, "Uninstall.exe") + "\" --silent");
                k.SetValue("EstimatedSize", (int)Math.Min(int.MaxValue, bytes / 1024), RegistryValueKind.DWord);
                k.SetValue("InstallDate", DateTime.Now.ToString("yyyyMMdd", CultureInfo.InvariantCulture));
                k.SetValue("NoModify", 1, RegistryValueKind.DWord);
                k.SetValue("NoRepair", 1, RegistryValueKind.DWord);
            }
            Log.Line("Apps entry HKCU\\" + Env.UninstallKeyRoot + Env.UninstallKeyName);
        }

        public static void CopyLog(string dir)
        {
            try
            {
                string logs = Path.Combine(dir, "logs");
                Directory.CreateDirectory(logs);
                File.WriteAllText(Path.Combine(logs, "setup.log"), Log.ThisRun, new UTF8Encoding(false));
            }
            catch (Exception e) { Log.Line("could not copy the log: " + e.Message); }
        }
    }

    public sealed class UninstallResult
    {
        public int Code;
        public string Message = "";
        public readonly List<string> Kept = new List<string>();
    }

    /// <summary>The uninstaller: what install.manifest names goes; the player's own files stay unless asked.</summary>
    public static class Uninstaller
    {
        public static readonly string[] KeptNames = { "saves", "ChiliNative.ini", "launcher.ini", "logs", "art", "textures", "texture_dump" };

        public static UninstallResult Run(string dir, bool removeAll, Action<int, int> progress = null)
        {
            var result = new UninstallResult();
            dir = dir.TrimEnd('\\');
            var record = InstallRecord.Load(dir);
            if (record == null)
            {
                result.Code = Exit.Other; result.Message = "There is no installation here.";
                Log.Error("no install.manifest in " + dir);
                return result;
            }
            if (Util.RunningFrom(dir) != null) { result.Code = Exit.Running; result.Message = Folder.TextRunning; Log.Error("the game or the launcher runs from " + dir); return result; }
            // Everything in the folder goes only from a folder Setup really installed the game into.
            if (removeAll && (dir.Length <= 3 || !record.Files.Any(f => string.Equals(f.Path, "ChiliConCarnage.exe", StringComparison.OrdinalIgnoreCase))))
            {
                Log.Line("this record names no game program: only its files are removed, nothing else of the folder");
                removeAll = false;
            }
            string self = Env.SelfPath;
            int n = 0, total = record.Files.Count + record.Shortcuts.Count;
            foreach (var f in record.Files)
            {
                if (progress != null) progress(++n, total);
                string file = Path.Combine(dir, f.Path.Replace('/', '\\'));
                if (string.Equals(Util.Full(file), Util.Full(self), StringComparison.OrdinalIgnoreCase)) continue;    // goes last
                try { if (File.Exists(file)) File.Delete(file); }
                catch (Exception e) { Log.Error("could not remove " + f.Path + ": " + e.Message); }
            }
            foreach (var s in record.Shortcuts)
            {
                if (progress != null) progress(++n, total);
                try { if (File.Exists(s)) File.Delete(s); } catch (Exception e) { Log.Error("could not remove " + s + ": " + e.Message); }
                Installer.RemoveShortcutFolder(Path.GetDirectoryName(s));
            }
            try { File.Delete(Path.Combine(dir, "ChiliNative.gpucache")); } catch { }
            foreach (var key in new[] { Env.UninstallKeyRoot + record.UninstallKey, record.SettingsKey })
            {
                if (string.IsNullOrEmpty(key) || key.EndsWith("\\")) continue;
                try { Registry.CurrentUser.DeleteSubKeyTree(key, false); Log.Line("removed HKCU\\" + key); }
                catch (Exception e) { Log.Error("registry " + key + ": " + e.Message); }
            }
            // HKCU\Software\ChiliConCarnagePC itself, when nothing else is in it.
            try
            {
                using (var k = Registry.CurrentUser.OpenSubKey(@"Software\ChiliConCarnagePC"))
                    if (k != null && k.SubKeyCount == 0 && k.ValueCount == 0) { k.Close(); Registry.CurrentUser.DeleteSubKey(@"Software\ChiliConCarnagePC", false); }
            }
            catch { }
            try { File.Delete(Path.Combine(dir, "install.manifest")); } catch { }
            if (removeAll)
            {
                // Everything in the folder: the saves, the settings, the logs, the player's packs. A disc image
                // or disc folder of the player's that lies in it is not Setup's to delete and stays.
                string disc = record.Disc.Length > 0 ? Util.Full(Path.IsPathRooted(record.Disc) ? record.Disc : Path.Combine(dir, record.Disc)) : null;
                foreach (var entry in Directory.GetFileSystemEntries(dir))
                {
                    string ext = Path.GetExtension(entry).ToLowerInvariant();
                    if (string.Equals(entry, self, StringComparison.OrdinalIgnoreCase)) continue;
                    if (ext == ".iso" || ext == ".cso" || (disc != null && (Util.SameOrInside(disc, entry)))) { result.Kept.Add(Path.GetFileName(entry)); continue; }
                    try
                    {
                        if (Directory.Exists(entry)) Directory.Delete(entry, true);
                        else File.Delete(entry);
                    }
                    catch (Exception e) { Log.Error("could not remove " + entry + ": " + e.Message); }
                }
                Log.Line("removed everything in " + dir + (result.Kept.Count > 0 ? " but " + string.Join(", ", result.Kept) : ""));
            }
            else
            {
                // Folders the removal emptied, deepest first; the player's files keep theirs.
                foreach (var sub in Directory.GetDirectories(dir, "*", SearchOption.AllDirectories).OrderByDescending(d => d.Length)) Installer.RemoveIfEmpty(sub);
                foreach (var entry in Directory.GetFileSystemEntries(dir))
                    if (!string.Equals(entry, self, StringComparison.OrdinalIgnoreCase)) result.Kept.Add(Path.GetFileName(entry));
                Log.Line("kept in " + dir + ": " + (result.Kept.Count > 0 ? string.Join(", ", result.Kept) : "nothing"));
            }
            result.Code = Exit.Done;
            return result;
        }

        /// <summary>
        /// The uninstaller cannot delete its own file while it runs: a hidden command waits until it has
        /// ended, then deletes it and the folder when that is empty.
        /// </summary>
        public static void RemoveSelfLater(string dir)
        {
            try
            {
                string self = Env.SelfPath;
                if (!string.Equals(Path.GetDirectoryName(self).TrimEnd('\\'), dir.TrimEnd('\\'), StringComparison.OrdinalIgnoreCase)) return;
                string command = "/c ping 127.0.0.1 -n 3 >nul & del /f /q \"" + self + "\" & rmdir \"" + dir.TrimEnd('\\') + "\"";
                Process.Start(new ProcessStartInfo(Path.Combine(Environment.SystemDirectory, "cmd.exe"), command)
                {
                    CreateNoWindow = true, UseShellExecute = false, WindowStyle = ProcessWindowStyle.Hidden, WorkingDirectory = Path.GetTempPath(),
                });
                Log.Line("Uninstall.exe and the empty folder are removed when this program has ended");
            }
            catch (Exception e) { Log.Line("could not arrange the removal of Uninstall.exe: " + e.Message); }
        }
    }
}
