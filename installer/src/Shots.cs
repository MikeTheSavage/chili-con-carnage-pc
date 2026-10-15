// --screenshot: every page and state drawn off screen to PNG files, with texts.txt listing every text each
// picture draws. Without --iso the disc's pictures are absent (nothing of the game is drawn); with --iso
// the player's disc is checked for real and its two pictures are shown as the window would show them.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using ChiliLauncher;

namespace ChiliSetup
{
    public static class Shots
    {
        public sealed class State
        {
            public string Name;
            public Action<SetupModel> Prepare;
            public string Focus;
            public Device Device;
            public PadKind Look;
        }

        /// <summary>Every state worth a picture. `disc` is the checked disc (or null for a made-up accepted one).</summary>
        public static List<State> All(DiscResult disc)
        {
            var good = disc != null && disc.Ok ? disc : new DiscResult { Verdict = DiscVerdict.Ok, Path = @"D:\Games\Chili Con Carnage (USA).iso", Size = 797081600, Id = Disc.Id };
            string iso = good.Path;
            string dir = @"C:\Users\Player\AppData\Local\Programs\Chili Con Carnage PC";
            Action<SetupModel> accepted = m =>
            {
                m.DiscPath = iso; m.Disc = good; m.SetPictures(good); m.MusicOn = true; m.Furthest = 3;
            };
            Action<SetupModel> folderOk = m =>
            {
                accepted(m);
                m.Dir = dir;
                m.Folder = new FolderCheck { Verdict = FolderVerdict.Ok, Dir = dir, Free = 31242874880, Need = m.Need };
            };
            Func<DiscVerdict, string, Action<SetupModel>> refused = (v, text) => m =>
            {
                m.DiscPath = v == DiscVerdict.Cso ? @"D:\Games\Chili Con Carnage (USA).cso" : v == DiscVerdict.WrongId ? @"D:\Games\Another Game.iso" : iso;
                m.Disc = new DiscResult { Verdict = v, Path = m.DiscPath, Message = text };
            };
            var list = new List<State>
            {
                new State { Name = "01_welcome", Prepare = m => { } },
                new State { Name = "01_welcome_damaged", Prepare = m => { m.Damaged = "The installer's data is damaged. Download it again."; } },
                new State { Name = "02_disc_empty", Prepare = m => { m.Page = Page.Disc; } },
                new State { Name = "02_disc_checking", Prepare = m => { m.Page = Page.Disc; m.DiscPath = iso; m.Checking = true; } },
                new State { Name = "02_disc_ok", Prepare = m => { m.Page = Page.Disc; accepted(m); } },
                new State { Name = "02_disc_wrong_id", Prepare = m => { m.Page = Page.Disc; refused(DiscVerdict.WrongId, "This is the disc ULES-00629, not Chili Con Carnage (USA, ULUS-10216).")(m); } },
                new State { Name = "02_disc_other_program", Prepare = m => { m.Page = Page.Disc; refused(DiscVerdict.OtherProgram, Disc.TextOtherProgram)(m); } },
                new State { Name = "02_disc_cso", Prepare = m => { m.Page = Page.Disc; refused(DiscVerdict.Cso, Disc.TextCso)(m); } },
                new State { Name = "02_disc_not_iso", Prepare = m => { m.Page = Page.Disc; refused(DiscVerdict.NotIso, Disc.TextNotIso)(m); } },
                new State { Name = "02_disc_incomplete", Prepare = m => { m.Page = Page.Disc; refused(DiscVerdict.Incomplete, "This disc is incomplete: assets00.naz is missing.")(m); } },
                new State { Name = "03_folder", Prepare = m => { m.Page = Page.Folder; folderOk(m); } },
                new State { Name = "03_folder_update", Prepare = m => { m.Page = Page.Folder; folderOk(m); m.Folder.Verdict = FolderVerdict.Update; m.Folder.Message = Folder.TextUpdate; } },
                new State { Name = "03_folder_not_empty", Prepare = m => { m.Page = Page.Folder; folderOk(m); m.Dir = @"D:\Games"; m.Folder = new FolderCheck { Verdict = FolderVerdict.NotEmpty, Dir = @"D:\Games", Free = 412316860416, Message = Folder.TextNotEmpty }; } },
                new State { Name = "03_folder_running", Prepare = m => { m.Page = Page.Folder; folderOk(m); m.Folder.Verdict = FolderVerdict.Running; m.Folder.Message = Folder.TextRunning; } },
                new State { Name = "03_folder_no_space", Prepare = m => { m.Page = Page.Folder; folderOk(m); m.Folder.Verdict = FolderVerdict.NoSpace; m.Folder.Free = 104857600; m.Folder.Message = Folder.TextNoSpace; } },
                new State { Name = "04_options", Prepare = m => { m.Page = Page.Options; folderOk(m); } },
                new State { Name = "04_options_no_pack", Prepare = m => { m.Page = Page.Options; folderOk(m); m.HasHd = false; m.HdLoading = false; } },
                new State { Name = "05_install_35", Prepare = m => { m.Page = Page.Install; folderOk(m); m.Installing = true; m.Total = m.Need - (50L << 20); m.Done = (long)(m.Total * 0.35); m.Current = "mods/hd_loading/textures/58634ac6722e2e6e.png"; } },
                new State { Name = "05_install_question", Prepare = m => { m.Page = Page.Install; folderOk(m); m.Installing = true; m.Total = m.Need - (50L << 20); m.Done = (long)(m.Total * 0.6); m.Current = "ChiliConCarnage.exe"; m.Ask = new Question { Text = "Stop the installation?", Buttons = new[] { "Stop", "Keep going" }, Cancel = 1, Focus = 1 }; } },
                new State { Name = "05_install_rolled_back", Prepare = m => { m.Page = Page.Install; folderOk(m); m.RolledBack = true; } },
                new State { Name = "06_done", Prepare = m => { m.Page = Page.Done; folderOk(m); m.Launch = false; m.Result = new InstallResult { Code = 0, Dir = dir }; } },
                new State { Name = "06_done_play", Prepare = m => { m.Page = Page.Done; folderOk(m); m.Result = new InstallResult { Code = 0, Dir = dir }; } },
                new State { Name = "06_done_warning", Prepare = m => { m.Page = Page.Done; folderOk(m); m.Updating = true; var r = new InstallResult { Code = 0, Dir = dir }; r.Warnings.Add("Could not write the shortcut."); m.Result = r; } },
                new State { Name = "06_not_installed", Prepare = m => { m.Page = Page.Done; folderOk(m); m.Result = new InstallResult { Code = Exit.WriteFailed, Dir = dir, Message = "Could not write ChiliConCarnage.exe." }; } },
                new State { Name = "07_uninstall", Prepare = m => { m.Uninstaller = true; m.Page = Page.Uninstall; m.Dir = dir; } },
                new State { Name = "07_uninstall_question", Prepare = m => { m.Uninstaller = true; m.Page = Page.Uninstall; m.Dir = dir; m.RemoveAll = true; m.Ask = new Question { Text = "Delete the saves too?", Buttons = new[] { "Delete", "Keep" }, Cancel = 1, Focus = 1 }; } },
                new State { Name = "07_uninstall_removing", Prepare = m => { m.Uninstaller = true; m.Page = Page.Uninstall; m.Dir = dir; m.Removing = 1; m.Done = 120; m.Total = 160; } },
                new State { Name = "07_uninstall_done", Prepare = m => { m.Uninstaller = true; m.Page = Page.Uninstall; m.Dir = dir; m.Removing = 2; } },
                new State { Name = "08_close_question", Prepare = m => { m.Ask = new Question { Text = "Close Setup?", Buttons = new[] { "Close", "Stay" }, Cancel = 1, Focus = 1 }; } },
            };
            // The controller and the keyboard: the prompts along the bottom and the focus ring.
            list.Add(new State { Name = "02_disc_ok_pad", Prepare = m => { m.Page = Page.Disc; accepted(m); }, Focus = "browse_iso", Device = Device.Pad, Look = PadKind.Xbox });
            list.Add(new State { Name = "04_options_pad", Prepare = m => { m.Page = Page.Options; folderOk(m); }, Focus = "toggle_desktop", Device = Device.Pad, Look = PadKind.PlayStation });
            list.Add(new State { Name = "04_options_keys", Prepare = m => { m.Page = Page.Options; folderOk(m); }, Focus = "volume", Device = Device.Keyboard });
            list.Add(new State { Name = "06_done_pad", Prepare = m => { m.Page = Page.Done; folderOk(m); m.Result = new InstallResult { Code = 0, Dir = dir }; }, Focus = "next", Device = Device.Pad, Look = PadKind.Xbox });
            list.Add(new State { Name = "07_uninstall_pad", Prepare = m => { m.Uninstaller = true; m.Page = Page.Uninstall; m.Dir = dir; }, Focus = "toggle_removeall", Device = Device.Pad, Look = PadKind.Xbox });
            return list;
        }

        /// <summary>One state drawn at this scale; the texts drawn are added to `texts` when given.</summary>
        public static BitmapSource Render(State s, double scale, List<string> texts)
        {
            var m = new SetupModel { Version = Program.Version.StartsWith("0.0") ? "0.9.0" : Program.Version };
            s.Prepare(m);
            var view = new SetupView(m) { Static = true, Width = SetupView.W, Height = SetupView.H };
            view.Measure(new Size(SetupView.W, SetupView.H));
            view.Arrange(new Rect(0, 0, SetupView.W, SetupView.H));
            view.UpdateLayout();
            // A first drawing tells where the controls are; then the focus, then the picture.
            using (var first = new DrawingVisual().RenderOpen()) view.Paint(first);
            m.Device = s.Device;
            m.PadLook = s.Look;
            if (s.Focus != null) { view.FocusId = s.Focus; view.ShowFocus = true; }
            if (m.Ask != null) { view.FocusId = "q" + m.Ask.Focus; view.ShowFocus = s.Device != Device.None; }
            Tx.Log = texts != null ? new List<string>() : null;
            var visual = new DrawingVisual();
            using (var dc = visual.RenderOpen()) view.Paint(dc);
            if (texts != null) { texts.AddRange(Tx.Log.Distinct()); Tx.Log = null; }
            var bitmap = new RenderTargetBitmap((int)Math.Round(SetupView.W * scale), (int)Math.Round(SetupView.H * scale), 96 * scale, 96 * scale, PixelFormats.Pbgra32);
            bitmap.Render(visual);
            bitmap.Freeze();
            return bitmap;
        }

        public static int Run(string outDir, double scale, string iso, string only)
        {
            Directory.CreateDirectory(outDir);
            Remembered.NoWrites = true;
            Faces.Load();
            Tx.PixelsPerDip = scale;
            DiscResult disc = null;
            if (!string.IsNullOrEmpty(iso))
            {
                disc = Disc.Check(iso, true);
                Console.WriteLine("disc " + disc.Path + ": " + (disc.Ok ? "accepted" : disc.Message));
            }
            var texts = new StringBuilder();
            texts.AppendLine("fonts: " + Faces.Loaded + "; scale " + scale.ToString(CultureInfo.InvariantCulture));
            int count = 0;
            foreach (var s in All(disc))
            {
                if (!string.IsNullOrEmpty(only) && !s.Name.StartsWith(only, StringComparison.OrdinalIgnoreCase)) continue;
                var drawn = new List<string>();
                var bitmap = Render(s, scale, drawn);
                var encoder = new PngBitmapEncoder();
                encoder.Frames.Add(BitmapFrame.Create(bitmap));
                string file = Path.Combine(outDir, s.Name + ".png");
                using (var f = File.Create(file)) encoder.Save(f);
                texts.AppendLine(s.Name);
                foreach (var t in drawn) texts.AppendLine("    " + t);
                Console.WriteLine(file);
                count++;
            }
            File.WriteAllText(Path.Combine(outDir, "texts.txt"), texts.ToString(), new UTF8Encoding(false));
            Console.WriteLine(count + " pictures at scale " + scale.ToString(CultureInfo.InvariantCulture) + " in " + outDir);
            return 0;
        }
    }
}
