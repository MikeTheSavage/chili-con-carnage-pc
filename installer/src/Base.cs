// Where things go (the user's folders, or a stand-in home for tests), the log, small helpers.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;

namespace ChiliSetup
{
    /// <summary>
    /// The places Setup writes to. With --home (tests) every per-user place is a folder under that home, the
    /// remembered values go to HKCU\Software\ChiliConCarnagePC\Test and the Apps entry is named
    /// ChiliConCarnagePC-Test, so a test never touches the real ones.
    /// </summary>
    public static class Env
    {
        public const string Product = "Chili Con Carnage PC";
        public const string FolderName = "Chili Con Carnage PC";
        public const string UninstallKeyRoot = @"Software\Microsoft\Windows\CurrentVersion\Uninstall\";
        public static string Home;
        public static bool IsTest { get { return Home != null; } }

        public static string LocalAppData
        {
            get { return IsTest ? Path.Combine(Home, "Local") : Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData); }
        }
        public static string DefaultDir { get { return Path.Combine(LocalAppData, "Programs", FolderName); } }
        public static string StartMenuPrograms
        {
            get { return IsTest ? Path.Combine(Home, "Roaming", "Microsoft", "Windows", "Start Menu", "Programs") : Environment.GetFolderPath(Environment.SpecialFolder.Programs); }
        }
        public static string StartMenuFolder { get { return Path.Combine(StartMenuPrograms, FolderName); } }
        public static string Desktop
        {
            get { return IsTest ? Path.Combine(Home, "Desktop") : Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory); }
        }
        public static string SettingsKey { get { return IsTest ? @"Software\ChiliConCarnagePC\Test" : @"Software\ChiliConCarnagePC\Setup"; } }
        public static string UninstallKeyName { get { return IsTest ? "ChiliConCarnagePC-Test" : "ChiliConCarnagePC"; } }
        public static string DefaultLog
        {
            get { return IsTest ? Path.Combine(Home, "ChiliConCarnagePC-Setup.log") : Path.Combine(Path.GetTempPath(), "ChiliConCarnagePC-Setup.log"); }
        }

        /// <summary>This program's own file.</summary>
        public static string SelfPath
        {
            get
            {
                try { return Process.GetCurrentProcess().MainModule.FileName; }
                catch { return typeof(Env).Assembly.Location; }
            }
        }

        static string temp;
        /// <summary>%TEMP%\ChiliConCarnagePC-Setup-&lt;pid&gt;: the fonts and the music tool while Setup runs.</summary>
        public static string Temp
        {
            get
            {
                if (temp == null)
                {
                    temp = Path.Combine(Path.GetTempPath(), "ChiliConCarnagePC-Setup-" + Process.GetCurrentProcess().Id);
                    Directory.CreateDirectory(temp);
                }
                return temp;
            }
        }
        /// <summary>Removes this run's temp folder (what Windows still holds open stays for the next run to remove).</summary>
        public static void RemoveTemp()
        {
            if (temp == null) return;
            try { Directory.Delete(temp, true); } catch { }
        }
        /// <summary>Leftovers of earlier runs that were killed: their folders, when that process is gone.</summary>
        public static void RemoveStaleTemps()
        {
            try
            {
                foreach (var dir in Directory.GetDirectories(Path.GetTempPath(), "ChiliConCarnagePC-Setup-*"))
                {
                    int pid;
                    string tail = Path.GetFileName(dir).Substring("ChiliConCarnagePC-Setup-".Length);
                    if (!int.TryParse(tail, NumberStyles.None, CultureInfo.InvariantCulture, out pid)) continue;
                    if (pid == Process.GetCurrentProcess().Id) continue;
                    bool alive;
                    try { using (var p = Process.GetProcessById(pid)) alive = !p.HasExited; } catch { alive = false; }
                    if (!alive) try { Directory.Delete(dir, true); } catch { }
                }
            }
            catch { }
        }
    }

    /// <summary>
    /// The log: %TEMP%\ChiliConCarnagePC-Setup.log, appended, one header line per run. After an install the
    /// lines of this run are copied to logs\setup.log in the game's folder. Never a save's name.
    /// </summary>
    public static class Log
    {
        static readonly object gate = new object();
        static string file;
        static readonly StringBuilder run = new StringBuilder();
        public static int Errors;
        public static string File { get { return file; } }
        /// <summary>Tests: lines go to memory only.</summary>
        public static bool Off;

        public static void Open(string path, string header)
        {
            file = path;
            Line("==== " + DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss", CultureInfo.InvariantCulture) + " " + header);
        }
        public static void Line(string text)
        {
            string line = DateTime.Now.ToString("HH:mm:ss.fff", CultureInfo.InvariantCulture) + " " + text;
            lock (gate)
            {
                run.Append(line).Append("\r\n");
                if (file == null || Off) return;
                try
                {
                    string dir = Path.GetDirectoryName(file);
                    if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
                    System.IO.File.AppendAllText(file, line + "\r\n", new UTF8Encoding(false));
                }
                catch { }
            }
        }
        public static void Error(string text) { Errors++; Line("error: " + text); }
        /// <summary>This run's lines.</summary>
        public static string ThisRun { get { lock (gate) return run.ToString(); } }
    }

    public static class Util
    {
        public static string Sha256(byte[] data, int length = -1)
        {
            using (var h = SHA256.Create()) return Hex(h.ComputeHash(data, 0, length < 0 ? data.Length : length));
        }
        public static string Sha256File(string file)
        {
            using (var h = SHA256.Create())
            using (var f = new FileStream(file, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20)) return Hex(h.ComputeHash(f));
        }
        public static string Hex(byte[] b)
        {
            var s = new StringBuilder(b.Length * 2);
            foreach (var x in b) s.Append(x.ToString("x2", CultureInfo.InvariantCulture));
            return s.ToString();
        }
        public static string Mb(long bytes) { return Math.Max(bytes > 0 ? 1 : 0, (long)Math.Round(bytes / 1048576.0)).ToString("N0", CultureInfo.InvariantCulture) + " MB"; }
        public static string Free(long bytes)
        {
            if (bytes >= 10L << 30) return (bytes / 1073741824.0).ToString("0.0", CultureInfo.InvariantCulture) + " GB";
            if (bytes >= 1L << 30) return (bytes / 1073741824.0).ToString("0.00", CultureInfo.InvariantCulture) + " GB";
            return Mb(bytes);
        }
        public static string Grouped(long n) { return n.ToString("N0", CultureInfo.InvariantCulture); }

        /// <summary>The full path, or null when it is not a usable path.</summary>
        public static string Full(string path)
        {
            if (string.IsNullOrWhiteSpace(path)) return null;
            try { return Path.GetFullPath(Environment.ExpandEnvironmentVariables(path.Trim().Trim('"'))); } catch { return null; }
        }
        public static bool SameOrInside(string path, string folder)
        {
            if (path == null || folder == null) return false;
            string p = path.TrimEnd('\\') + "\\", f = folder.TrimEnd('\\') + "\\";
            return p.StartsWith(f, StringComparison.OrdinalIgnoreCase);
        }

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern bool GetDiskFreeSpaceEx(string directory, out ulong freeToCaller, out ulong total, out ulong totalFree);
        /// <summary>Free bytes for this user on the volume that holds `path` (or its nearest existing parent); -1 when unknown.</summary>
        public static long FreeSpace(string path)
        {
            string p = path;
            while (!string.IsNullOrEmpty(p) && !Directory.Exists(p)) p = Path.GetDirectoryName(p);
            if (string.IsNullOrEmpty(p)) return -1;
            ulong free, total, all;
            if (!GetDiskFreeSpaceEx(p.TrimEnd('\\') + "\\", out free, out total, out all)) return -1;
            return (long)Math.Min(free, (ulong)long.MaxValue);
        }

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern bool QueryFullProcessImageName(IntPtr process, int flags, StringBuilder name, ref int size);
        [DllImport("kernel32.dll")]
        static extern bool CloseHandle(IntPtr handle);
        /// <summary>The program file of a running process, or null.</summary>
        public static string ImageOf(int pid)
        {
            IntPtr h = OpenProcess(0x1000 /* PROCESS_QUERY_LIMITED_INFORMATION */, false, pid);
            if (h == IntPtr.Zero) return null;
            try
            {
                var sb = new StringBuilder(1024);
                int size = sb.Capacity;
                return QueryFullProcessImageName(h, 0, sb, ref size) ? sb.ToString(0, size) : null;
            }
            finally { CloseHandle(h); }
        }
        /// <summary>A program running from this folder (the game or the launcher), or null.</summary>
        public static string RunningFrom(string folder)
        {
            if (string.IsNullOrEmpty(folder)) return null;
            int self = Process.GetCurrentProcess().Id;
            foreach (var p in Process.GetProcesses())
            {
                try
                {
                    if (p.Id == self || p.Id == 0 || p.Id == 4) continue;
                    string image = ImageOf(p.Id);
                    if (image != null && SameOrInside(image, folder) && !string.Equals(Path.GetFileName(image), "Uninstall.exe", StringComparison.OrdinalIgnoreCase))
                        return Path.GetFileName(image);
                }
                catch { }
                finally { p.Dispose(); }
            }
            return null;
        }

        /// <summary>The name of the Windows build, for the log.</summary>
        public static string WindowsVersion()
        {
            try
            {
                using (var k = Microsoft.Win32.Registry.LocalMachine.OpenSubKey(@"SOFTWARE\Microsoft\Windows NT\CurrentVersion"))
                    if (k != null)
                    {
                        string name = Convert.ToString(k.GetValue("ProductName") ?? "Windows"), build = Convert.ToString(k.GetValue("CurrentBuildNumber") ?? "");
                        int number;
                        // Windows 11 still names itself Windows 10 there.
                        if (int.TryParse(build, out number) && number >= 22000) name = name.Replace("Windows 10", "Windows 11");
                        return name + " " + (k.GetValue("DisplayVersion") ?? k.GetValue("ReleaseId") ?? "") + " build " + build + "." + (k.GetValue("UBR") ?? "");
                    }
            }
            catch { }
            return Environment.OSVersion.VersionString;
        }

        /// <summary>A command line's argument, quoted when it needs to be.</summary>
        public static string Quote(string s)
        {
            if (string.IsNullOrEmpty(s)) return "\"\"";
            if (s.IndexOfAny(new[] { ' ', '\t', '"' }) < 0) return s;
            var sb = new StringBuilder("\"");
            int slashes = 0;
            foreach (char c in s)
            {
                if (c == '\\') { slashes++; continue; }
                if (c == '"') sb.Append('\\', slashes * 2 + 1);
                else sb.Append('\\', slashes);
                slashes = 0;
                sb.Append(c);
            }
            sb.Append('\\', slashes * 2);
            return sb.Append('"').ToString();
        }
    }

    /// <summary>Setup's own remembered values (volume, mute, the last folder and disc), in the current user's registry.</summary>
    public static class Remembered
    {
        public static int Get(string name, int fallback)
        {
            try
            {
                using (var k = Microsoft.Win32.Registry.CurrentUser.OpenSubKey(Env.SettingsKey))
                {
                    var v = k == null ? null : k.GetValue(name);
                    if (v is int) return (int)v;
                }
            }
            catch { }
            return fallback;
        }
        public static string GetText(string name)
        {
            try { using (var k = Microsoft.Win32.Registry.CurrentUser.OpenSubKey(Env.SettingsKey)) return k == null ? null : k.GetValue(name) as string; }
            catch { return null; }
        }
        public static void Set(string name, object value)
        {
            if (NoWrites) return;
            try
            {
                using (var k = Microsoft.Win32.Registry.CurrentUser.CreateSubKey(Env.SettingsKey))
                {
                    if (value is int) k.SetValue(name, value, Microsoft.Win32.RegistryValueKind.DWord);
                    else k.SetValue(name, value ?? "", Microsoft.Win32.RegistryValueKind.String);
                }
            }
            catch (Exception e) { Log.Line("could not remember " + name + ": " + e.Message); }
        }
        /// <summary>Screenshots and tests that must not change what a player's Setup remembers.</summary>
        public static bool NoWrites;
    }
}
