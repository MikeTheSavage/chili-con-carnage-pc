// What the window shows: the pages, the state of each, the typefaces.
using System;
using System.Collections.Generic;
using System.IO;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using ChiliLauncher;

namespace ChiliSetup
{
    /// <summary>
    /// The three typefaces Setup carries (SIL Open Font License 1.1, installer/fonts/OFL.txt): Changa One
    /// Italic for the display lettering, Tektur for numbers, Saira Italic for labels and lines. They are
    /// written to Setup's temp folder and loaded from there; Segoe UI stands in for any that cannot be used.
    /// Paths are drawn in Segoe UI.
    /// </summary>
    public static class Faces
    {
        public static Typeface Display = ChiliLauncher.Fonts.Heading, Numbers = ChiliLauncher.Fonts.Semi, Label = ChiliLauncher.Fonts.Semi, Line = ChiliLauncher.Fonts.Ui, Path = ChiliLauncher.Fonts.Ui, PathSemi = ChiliLauncher.Fonts.Semi;
        public static readonly string[] Files = { "ChangaOne-Italic.ttf", "Tektur-wdth-wght.ttf", "Saira-Italic-wdth-wght.ttf" };
        public static string Loaded = "Segoe UI only";

        public static void Load()
        {
            try
            {
                string dir = System.IO.Path.Combine(Env.Temp, "fonts");
                Directory.CreateDirectory(dir);
                foreach (var name in Files)
                {
                    using (var s = typeof(Faces).Assembly.GetManifestResourceStream("font:" + name))
                    {
                        if (s == null) continue;
                        string target = System.IO.Path.Combine(dir, name);
                        if (File.Exists(target) && new FileInfo(target).Length == s.Length) continue;
                        using (var o = File.Create(target)) s.CopyTo(o);
                    }
                }
                var uri = new Uri(dir.TrimEnd('\\') + "\\");
                var parts = new List<string>();
                Typeface t;
                if (Try(uri, "Changa One", FontWeights.Normal, FontStyles.Italic, out t)) { Display = t; parts.Add("Changa One"); }
                if (Try(uri, "Tektur", FontWeights.Normal, FontStyles.Normal, out t)) { Numbers = t; parts.Add("Tektur"); }
                // Saira is a variable font whose default is Thin: its SemiBold and Medium are used only when
                // Windows offers them as such (named instances), else Segoe UI.
                Typeface semi, medium;
                if (Try(uri, "Saira", FontWeights.SemiBold, FontStyles.Italic, out semi) && Try(uri, "Saira", FontWeights.Medium, FontStyles.Italic, out medium))
                {
                    Label = semi; Line = medium; parts.Add("Saira");
                }
                Loaded = parts.Count > 0 ? string.Join(", ", parts) : "Segoe UI only";
            }
            catch (Exception e) { Log.Line("fonts: " + e.Message); }
        }

        static bool Try(Uri dir, string family, FontWeight weight, FontStyle style, out Typeface face)
        {
            face = null;
            try
            {
                var t = new Typeface(new FontFamily(dir, "./#" + family), style, weight, FontStretches.Normal);
                GlyphTypeface g;
                if (!t.TryGetGlyphTypeface(out g)) return false;
                if (!g.FamilyNames.Values.Contains(family) && !string.Join("|", g.FamilyNames.Values).StartsWith(family)) return false;
                if (g.Weight != weight || g.StyleSimulations != StyleSimulations.None) return false;
                face = t;
                return true;
            }
            catch { return false; }
        }
    }

    public enum Page { Welcome, Disc, Folder, Options, Install, Done, Uninstall }
    public enum Device { None, Mouse, Keyboard, Pad }

    public sealed class Question
    {
        public string Text;
        public string[] Buttons;       // the first does the thing asked about, the Cancel one keeps things as they are
        public int Focus, Cancel;
        public bool Danger;            // the first button deletes or stops something: drawn in chili red
        public Action<int> Answer;
    }

    public sealed class SetupModel
    {
        public static readonly string[] Steps = { "Welcome", "Your disc", "Folder", "Options", "Install", "Done" };
        public bool Uninstaller;
        public Page Page = Page.Welcome;
        public int Furthest;
        public string Version = "0.9.0";
        public string Damaged;                     // the payload could not be read: the one line to show
        // Disc
        public string DiscPath = "";
        public DiscResult Disc;
        public bool Checking;
        public BitmapSource Icon0, Pic1, Pic1Soft;
        // Folder
        public string Dir = "";
        public FolderCheck Folder;
        public bool FolderChecking;
        // Options
        public bool HdLoading = true, Desktop = true, StartMenu = true, Launch = true;
        public bool HasHd = true;
        public long ProgramBytes = 110171811, HdBytes = 180534644;  // the 0.9.0 payload's parts; Setup reads its own
        // Install
        public long Done, Total = 1;
        public string Current = "";
        public bool Installing, Updating, RolledBack;
        public InstallResult Result;
        // Uninstall
        public bool RemoveAll;
        public int Removing;                       // 0 asking, 1 removing, 2 removed, 3 failed
        public string RemoveMessage = "";
        public List<string> Kept = new List<string>();
        // Music
        public bool MusicOn;
        public int Volume = 60;
        public bool Muted;
        // Window
        public Question Ask;
        public string Message;
        public bool DragOver;
        public Device Device;
        public PadKind PadLook = PadKind.Xbox;

        public long Need { get { return ProgramBytes + (HdLoading && HasHd ? HdBytes : 0) + (50L << 20); } }
        public bool DiscOk { get { return Disc != null && Disc.Ok && !Checking; } }
        public bool FolderOk { get { return Folder != null && Folder.Ok && !FolderChecking; } }
        public bool Update { get { return Folder != null && Folder.Verdict == FolderVerdict.Update; } }
        public bool Failed { get { return Result != null && Result.Code != Exit.Done; } }
        public int StepIndex
        {
            get
            {
                switch (Page)
                {
                    case Page.Welcome: return 0;
                    case Page.Disc: return 1;
                    case Page.Folder: return 2;
                    case Page.Options: return 3;
                    case Page.Install: return 4;
                    default: return 5;
                }
            }
        }

        /// <summary>The disc's two pictures from the bytes read off it (memory only).</summary>
        public void SetPictures(DiscResult d)
        {
            Icon0 = Picture(d != null ? d.Icon0 : null);
            Pic1 = Picture(d != null ? d.Pic1 : null);
            Pic1Soft = Pic1 != null ? ArtFolder.Soft(Pic1, 0.9) : null;
        }

        static BitmapSource Picture(byte[] bytes)
        {
            if (bytes == null || bytes.Length == 0) return null;
            try
            {
                var b = new BitmapImage();
                b.BeginInit();
                b.CacheOption = BitmapCacheOption.OnLoad;
                b.CreateOptions = BitmapCreateOptions.IgnoreColorProfile;
                b.StreamSource = new MemoryStream(bytes);
                b.EndInit();
                b.Freeze();
                return b;
            }
            catch { return null; }
        }
    }
}
