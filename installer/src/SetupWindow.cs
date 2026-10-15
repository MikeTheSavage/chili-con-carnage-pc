// The window: hosts the picture, feeds it the mouse, the keyboard and the controller, runs the checks and the
// install on worker threads, plays the music. The same window is the uninstaller.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Threading;
using System.Windows;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using ChiliLauncher;

namespace ChiliSetup
{
    public sealed class SetupWindow : Window
    {
        public readonly SetupModel M;
        public readonly SetupView View;
        readonly Payload payload;
        public readonly Music Music;
        readonly DispatcherTimer timer;
        readonly Stopwatch clock = Stopwatch.StartNew();
        readonly HidPads hid = new HidPads();
        readonly PadArbiter arbiter = new PadArbiter();
        IntPtr handle;
        Installer installer;
        bool closing, closeNow;
        public bool Quiet;
        /// <summary>Tests: the controller's state comes from the test, not from the devices.</summary>
        public PadState? FakePad;
        public int ExitCode;
        public string UninstallDir;
        bool removeSelfAtEnd;

        public SetupWindow(SetupModel m, Payload p)
        {
            M = m;
            payload = p;
            Music = new Music();
            M.Volume = Music.Volume;
            M.Muted = Music.Muted;
            Music.Changed = () => Dispatcher.BeginInvoke(new Action(() => { M.MusicOn = Music.Playing; M.Volume = Music.Volume; M.Muted = Music.Muted; View.InvalidateVisual(); }));
            Title = m.Uninstaller ? Env.Product + " Uninstall" : Env.Product + " Setup " + m.Version;
            Background = new SolidColorBrush(Pal.RustDark);
            UseLayoutRounding = true;
            ResizeMode = ResizeMode.CanMinimize;
            try
            {
                var stream = typeof(SetupWindow).Assembly.GetManifestResourceStream("icon");
                if (stream != null) Icon = BitmapFrame.Create(stream, BitmapCreateOptions.None, BitmapCacheOption.OnLoad);
            }
            catch { }
            View = new SetupView(m);
            Content = View;
            // 1024 x 640 units; on a small desktop 800 x 500.
            var area = SystemParameters.WorkArea;
            double w = SetupView.W, h = SetupView.H;
            if (area.Width < 1064 || area.Height < 720) { w = 800; h = 500; }
            View.Width = w; View.Height = h;
            SizeToContent = SizeToContent.WidthAndHeight;
            WindowStartupLocation = WindowStartupLocation.CenterScreen;

            View.Activated = Act;
            View.SliderMoved = (id, f) => { if (M.MusicOn) Music.SetVolume((int)(Math.Round(Math.Max(0, Math.Min(1, f)) * 20) * 5)); };
            View.Dropped = path => CheckDisc(path);

            timer = new DispatcherTimer(DispatcherPriority.Input) { Interval = TimeSpan.FromMilliseconds(16) };
            timer.Tick += (s, e) => Tick();
            SourceInitialized += (s, e) =>
            {
                handle = new WindowInteropHelper(this).Handle;
                Win32.DarkTitleBar(handle);
                var source = HwndSource.FromHwnd(handle);
                if (source != null) source.AddHook(WindowMessage);
                try { hid.Attach(handle); } catch { }
            };
            Loaded += (s, e) => { timer.Start(); if (!Quiet) View.Focus(); };
            Closing += OnClosing;
            Closed += (s, e) =>
            {
                timer.Stop();
                if (removeSelfAtEnd && UninstallDir != null) Uninstaller.RemoveSelfLater(UninstallDir);
            };
        }

        IntPtr WindowMessage(IntPtr hwnd, int message, IntPtr wParam, IntPtr lParam, ref bool handled)
        {
            if (message == HidPads.WmInput) hid.Input(lParam);
            else if (message == HidPads.WmInputDeviceChange) hid.Scan();
            return IntPtr.Zero;
        }

        bool InFront()
        {
            try { return handle != IntPtr.Zero && Win32.GetForegroundWindow() == handle; } catch { return IsActive; }
        }

        // ------------------------------------------------------------------ the controller

        double nextRepeat;
        int repeatDir;
        ushort lastButtons;

        void Tick()
        {
            bool front = InFront();
            if (front != View.WindowActive) { View.WindowActive = front; View.InvalidateVisual(); }
            double now = clock.Elapsed.TotalSeconds;
            PadState pad;
            if (FakePad.HasValue) pad = FakePad.Value;
            else if (front) pad = arbiter.Choose(XInput.Read(now), hid.Read(now, true), now);
            else pad = new PadState();
            Pad(pad, now);
            if (View.Moving) View.InvalidateVisual();
        }

        const ushort A = 0x1000, B = 0x2000, X = 0x4000, Y = 0x8000, Start = 0x10, Up = 0x1, Down = 0x2, PadLeft = 0x4, PadRight = 0x8;

        void Pad(PadState s, double now)
        {
            if (!s.Connected) { lastButtons = 0; repeatDir = 0; return; }
            ushort buttons = s.Buttons;
            // The left stick is the D-pad too.
            if (s.LY > 16000) buttons |= Up; else if (s.LY < -16000) buttons |= Down;
            if (s.LX < -16000) buttons |= PadLeft; else if (s.LX > 16000) buttons |= PadRight;
            ushort pressed = (ushort)(buttons & ~lastButtons);
            lastButtons = buttons;
            if (pressed != 0)
            {
                M.Device = Device.Pad;
                M.PadLook = s.Kind;
                View.ShowFocus = true;
            }
            if ((pressed & A) != 0) PadSelect();
            if ((pressed & B) != 0) Escape();
            if ((pressed & Y) != 0) Browse();
            if ((pressed & X) != 0) Mute();
            if ((pressed & Start) != 0) { if (M.Ask != null) PadSelect(); else Act("next"); }
            int dir = (buttons & Up) != 0 ? 1 : (buttons & Down) != 0 ? 2 : (buttons & PadLeft) != 0 ? 3 : (buttons & PadRight) != 0 ? 4 : 0;
            if (dir == 0) repeatDir = 0;
            else if (dir != repeatDir) { repeatDir = dir; nextRepeat = now + 0.4; Direction(dir); }
            else if (now >= nextRepeat) { nextRepeat = now + 0.12; Direction(dir); }
            if (pressed != 0) View.InvalidateVisual();
        }

        void Direction(int dir)
        {
            View.ShowFocus = true;
            var f = View.Focused;
            if (f == null) { View.EnsureFocus(); View.InvalidateVisual(); return; }
            if (dir == 3 || dir == 4)
            {
                if (f.Kind == HotKind.Slider) { Music.SetVolume(Music.Volume + (dir == 4 ? 5 : -5)); return; }
                if (f.Kind == HotKind.Toggle) { SetToggle(f.Id, dir == 4); return; }
            }
            View.MoveFocus(dir == 1 || dir == 3 ? -1 : 1);
        }

        void PadSelect()
        {
            var f = View.Focused;
            if (M.Ask != null) { View.EnsureFocus(); f = View.Focused; }
            if (f != null) Act(f.Id);
            else Act("next");
        }

        protected override void OnPreviewKeyDown(KeyEventArgs e)
        {
            Key key = e.Key == Key.System ? e.SystemKey : e.Key == Key.ImeProcessed ? e.ImeProcessedKey : e.Key;
            if (key == Key.F4 && (Keyboard.Modifiers & ModifierKeys.Alt) != 0) { base.OnPreviewKeyDown(e); return; }
            if (HandleKey(key, Keyboard.Modifiers)) e.Handled = true;
            base.OnPreviewKeyDown(e);
        }

        /// <summary>A key, from the window or from a test.</summary>
        public bool HandleKey(Key key, ModifierKeys mods)
        {
            if (key == Key.LeftShift || key == Key.RightShift || key == Key.LeftCtrl || key == Key.RightCtrl || key == Key.LeftAlt || key == Key.RightAlt) return false;
            M.Device = Device.Keyboard;
            var f = View.Focused;
            bool handled = true;
            switch (key)
            {
                case Key.Tab: View.MoveFocus((mods & ModifierKeys.Shift) != 0 ? -1 : 1); break;
                case Key.Up: View.MoveFocus(-1); break;
                case Key.Down: View.MoveFocus(1); break;
                case Key.Left:
                case Key.Right:
                    View.ShowFocus = true;
                    if (f != null && f.Kind == HotKind.Slider) Music.SetVolume(Music.Volume + (key == Key.Right ? 5 : -5));
                    else if (f != null && f.Kind == HotKind.Toggle) SetToggle(f.Id, key == Key.Right);
                    else View.MoveFocus(key == Key.Right ? 1 : -1);
                    break;
                case Key.Enter:
                    if (M.Ask != null) { View.EnsureFocus(); Act(View.Focused != null ? View.Focused.Id : "q" + M.Ask.Focus); break; }
                    if (f != null && (f.Kind == HotKind.Button || f.Kind == HotKind.Primary || f.Kind == HotKind.Field || f.Kind == HotKind.Rail || f.Kind == HotKind.Mute)) Act(f.Id);
                    else Act("next");
                    break;
                case Key.Space:
                    if (f != null) Act(f.Id);
                    break;
                case Key.Escape: Escape(); break;
                case Key.F: Browse(); break;
                case Key.M: Mute(); break;
                default: handled = false; break;
            }
            View.InvalidateVisual();
            return handled;
        }

        void Escape()
        {
            if (M.Ask != null) { Answer(M.Ask.Cancel); return; }
            switch (M.Page)
            {
                case Page.Welcome: Ask("Close Setup?", new[] { "Close", "Stay" }, 1, a => { if (a == 0) Close(); }); break;
                case Page.Install: if (M.Installing) Act("back"); else if (M.RolledBack) Close(); break;
                case Page.Done: Close(); break;
                case Page.Uninstall: if (M.Removing != 1) Close(); break;
                default: Act("back"); break;
            }
        }
        void Browse()
        {
            if (M.Ask != null) return;
            if (M.Page == Page.Disc) Act("browse_iso");
            else if (M.Page == Page.Folder) Act("browse_dir");
        }
        void Mute() { if (M.MusicOn) Music.ToggleMute(); }

        void Ask(string text, string[] buttons, int cancel, Action<int> answer)
        {
            M.Ask = new Question { Text = text, Buttons = buttons, Cancel = cancel, Focus = cancel, Answer = answer };
            View.FocusId = "q" + cancel;
            View.InvalidateVisual();
        }
        void Answer(int i)
        {
            var q = M.Ask;
            M.Ask = null;
            View.FocusId = null;
            View.InvalidateVisual();
            if (q != null && q.Answer != null) q.Answer(i);
        }

        void SetToggle(string id, bool on)
        {
            switch (id)
            {
                case "toggle_hd": if (M.HasHd) M.HdLoading = on; break;
                case "toggle_desktop": M.Desktop = on; break;
                case "toggle_startmenu": M.StartMenu = on; break;
                case "toggle_launch": M.Launch = on; break;
                case "toggle_removeall": M.RemoveAll = on; break;
            }
            View.InvalidateVisual();
        }
        bool ToggleValue(string id)
        {
            switch (id)
            {
                case "toggle_hd": return M.HdLoading;
                case "toggle_desktop": return M.Desktop;
                case "toggle_startmenu": return M.StartMenu;
                case "toggle_launch": return M.Launch;
                case "toggle_removeall": return M.RemoveAll;
            }
            return false;
        }

        /// <summary>What a control does.</summary>
        public void Act(string id)
        {
            if (id == null) return;
            if (M.Ask != null)
            {
                if (id.StartsWith("q")) Answer(int.Parse(id.Substring(1)));
                return;
            }
            if (id.StartsWith("toggle_")) { SetToggle(id, !ToggleValue(id)); return; }
            if (id.StartsWith("rail"))
            {
                int step = int.Parse(id.Substring(4));
                if (!M.Installing && step < M.StepIndex && M.Page != Page.Done) Go((Page)step);
                return;
            }
            switch (id)
            {
                case "next": Next(); break;
                case "back": Back(); break;
                case "browse_iso": case "path_iso": PickIso(); break;
                case "browse_disc_folder": PickDiscFolder(); break;
                case "browse_dir": case "path_dir": PickDir(); break;
                case "check_again": CheckFolder(); break;
                case "show_log": ShowLog(); break;
                case "mute": Mute(); break;
                case "volume": break;
            }
            View.InvalidateVisual();
        }

        void Go(Page page)
        {
            M.Page = page;
            M.Furthest = Math.Max(M.Furthest, M.StepIndex);
            M.Message = null;
            View.PageChanged();
            if (page == Page.Disc && string.IsNullOrEmpty(M.DiscPath))
            {
                string last = Remembered.GetText("LastIso");
                if (!string.IsNullOrEmpty(last) && (File.Exists(last) || Directory.Exists(last))) CheckDisc(last);
            }
            if (page == Page.Folder) CheckFolder();
        }

        void Next()
        {
            switch (M.Page)
            {
                case Page.Welcome: if (M.Damaged == null) Go(Page.Disc); break;
                case Page.Disc: if (M.DiscOk) Go(Page.Folder); break;
                case Page.Folder:
                    if (!M.FolderOk) break;
                    // Checked again on the way out: the game may have been started meanwhile.
                    var again = Folder.Check(M.Dir, M.Need);
                    M.Folder = again;
                    if (again.Ok) Go(Page.Options);
                    break;
                case Page.Options: StartInstall(); break;
                case Page.Install: if (M.RolledBack) Close(); break;
                case Page.Done:
                    if (!M.Failed && M.Launch) Play();
                    else Close();
                    break;
                case Page.Uninstall:
                    if (M.Removing == 0)
                    {
                        if (M.RemoveAll) Ask("Delete the saves too?", new[] { "Delete", "Keep" }, 1, a => { if (a == 0) StartRemove(); else { M.RemoveAll = false; View.InvalidateVisual(); } });
                        else StartRemove();
                    }
                    else if (M.Removing >= 2) Close();
                    break;
            }
        }

        void Back()
        {
            switch (M.Page)
            {
                case Page.Welcome: Close(); break;
                case Page.Disc: Go(Page.Welcome); break;
                case Page.Folder: Go(Page.Disc); break;
                case Page.Options: Go(Page.Folder); break;
                case Page.Install:
                    if (M.Installing) Ask("Stop the installation?", new[] { "Stop", "Keep going" }, 1, a => { if (a == 0 && installer != null) installer.Cancel = true; });
                    break;
                case Page.Uninstall: if (M.Removing != 1) Close(); break;
            }
        }

        // ------------------------------------------------------------------ the disc

        int discRun;
        public void CheckDisc(string path)
        {
            if (string.IsNullOrWhiteSpace(path)) return;
            M.DiscPath = path;
            M.Checking = true;
            M.Disc = null;
            M.SetPictures(null);
            View.InvalidateVisual();
            int mine = ++discRun;
            ThreadPool.QueueUserWorkItem(_ =>
            {
                var clockCheck = Stopwatch.StartNew();
                var r = Disc.Check(path, true);
                Log.Line("disc " + r.Path + ": " + (r.Ok ? "accepted" : r.Verdict + " (" + r.Message + ")") + ", id " + (r.Id.Length > 0 ? r.Id : "-") +
                    ", BOOT.BIN " + r.BootSize + " bytes fnv1a64 " + r.BootHash.ToString("x16") + ", " + clockCheck.ElapsedMilliseconds + " ms");
                Dispatcher.BeginInvoke(new Action(() =>
                {
                    if (mine != discRun) return;
                    M.Checking = false;
                    M.Disc = r;
                    M.DiscPath = r.Path;
                    if (r.Ok)
                    {
                        M.SetPictures(r);
                        Music.StartFor(payload, r.Path);
                    }
                    else Music.Stop(0.3);
                    View.InvalidateVisual();
                }));
            });
        }

        void PickIso()
        {
            var dialog = new Microsoft.Win32.OpenFileDialog
            {
                Title = "Choose the .iso file of the disc",
                Filter = "PSP disc image (*.iso)|*.iso|All files (*.*)|*.*",
                CheckFileExists = true,
            };
            try { if (!string.IsNullOrEmpty(M.DiscPath) && File.Exists(M.DiscPath)) dialog.InitialDirectory = Path.GetDirectoryName(M.DiscPath); } catch { }
            if (dialog.ShowDialog(this) == true) CheckDisc(dialog.FileName);
        }
        void PickDiscFolder()
        {
            string picked = FolderPicker.Show(handle, "Choose the folder of the disc");
            if (picked != null) CheckDisc(picked);
        }

        // ------------------------------------------------------------------ the folder

        int folderRun;
        public void CheckFolder()
        {
            if (string.IsNullOrEmpty(M.Dir)) M.Dir = DefaultDir();
            M.FolderChecking = true;
            string dir = M.Dir;
            long need = M.Need;
            int mine = ++folderRun;
            ThreadPool.QueueUserWorkItem(_ =>
            {
                var c = Folder.Check(dir, need);
                Dispatcher.BeginInvoke(new Action(() =>
                {
                    if (mine != folderRun) return;
                    M.FolderChecking = false;
                    M.Folder = c;
                    if (c.Dir.Length > 0) M.Dir = c.Dir;
                    View.InvalidateVisual();
                }));
            });
            View.InvalidateVisual();
        }

        public static string DefaultDir()
        {
            string last = Remembered.GetText("LastFolder");
            if (!string.IsNullOrEmpty(last) && Folder.HoldsInstall(last)) return last;
            return Env.DefaultDir;
        }

        void PickDir()
        {
            string picked = FolderPicker.Show(handle, "Choose the folder");
            if (picked == null) return;
            // A folder that is not empty and holds no game gets a folder of the game's own name inside it.
            if (Directory.Exists(picked) && !Folder.HoldsInstall(picked) && Directory.EnumerateFileSystemEntries(picked).Any() &&
                !string.Equals(Path.GetFileName(picked.TrimEnd('\\')), Env.FolderName, StringComparison.OrdinalIgnoreCase))
                picked = Path.Combine(picked, Env.FolderName);
            M.Dir = picked;
            CheckFolder();
        }

        // ------------------------------------------------------------------ the install

        void StartInstall()
        {
            if (payload == null) return;
            var options = new InstallOptions { Dir = M.Dir, Disc = M.Disc != null ? M.Disc.Path : M.DiscPath, HdLoading = M.HdLoading && M.HasHd, Desktop = M.Desktop, StartMenu = M.StartMenu, Launch = M.Launch };
            installer = new Installer(payload, options);
            M.Updating = M.Update;
            M.Installing = true;
            M.RolledBack = false;
            M.Done = 0;
            M.Total = Math.Max(1, installer.Need - (50L << 20));
            M.Current = "";
            Go(Page.Install);
            long lastShown = -1;
            installer.Progress = (done, total, file) =>
            {
                long tick = Environment.TickCount;
                if (file.Length > 0 && tick - lastShown < 50 && done < total) return;
                lastShown = tick;
                Dispatcher.BeginInvoke(new Action(() => { M.Done = done; M.Total = Math.Max(1, total); if (file.Length > 0) M.Current = file; View.InvalidateVisual(); }));
            };
            var worker = new Thread(() =>
            {
                var result = installer.Run();
                Dispatcher.BeginInvoke(new Action(() => InstallEnded(result)));
            }) { IsBackground = true, Name = "install" };
            worker.Start();
        }

        void InstallEnded(InstallResult result)
        {
            M.Installing = false;
            installer = null;
            M.Ask = null;
            ExitCode = result.Code;
            if (result.Code == Exit.Cancelled) { M.RolledBack = true; View.InvalidateVisual(); return; }
            M.Result = result;
            if (result.Code == Exit.NoSpace || result.Code == Exit.Running || result.Code == Exit.FolderRefused)
            {
                // Back to the folder, which is checked again; the message line says what went wrong.
                M.Result = null;
                Go(Page.Folder);
                M.Message = result.Message;
                return;
            }
            Go(Page.Done);
        }

        void Play()
        {
            string dir = M.Result != null ? M.Result.Dir : M.Dir;
            string launcher = Path.Combine(dir, "ChiliConCarnageLauncher.exe");
            Music.StopNow(0.5);
            try
            {
                Process.Start(new ProcessStartInfo(launcher) { UseShellExecute = false, WorkingDirectory = dir });
                Log.Line("started " + launcher);
            }
            catch (Exception e) { Log.Error("could not start the launcher: " + e.Message); }
            Close();
        }

        void ShowLog()
        {
            try { Process.Start(new ProcessStartInfo(Path.Combine(Environment.SystemDirectory, "notepad.exe"), Util.Quote(Log.File)) { UseShellExecute = false }); }
            catch (Exception e) { Log.Line("could not open the log: " + e.Message); }
        }

        // ------------------------------------------------------------------ the uninstall

        void StartRemove()
        {
            M.Removing = 1;
            M.Done = 0; M.Total = 1;
            View.InvalidateVisual();
            string dir = UninstallDir;
            bool all = M.RemoveAll;
            var worker = new Thread(() =>
            {
                var r = Uninstaller.Run(dir, all, (n, total) => Dispatcher.BeginInvoke(new Action(() => { M.Done = n; M.Total = Math.Max(1, total); View.InvalidateVisual(); })));
                Dispatcher.BeginInvoke(new Action(() =>
                {
                    ExitCode = r.Code;
                    M.Removing = r.Code == Exit.Done ? 2 : 3;
                    M.RemoveMessage = r.Message;
                    M.Kept = r.Kept;
                    if (r.Code == Exit.Done) removeSelfAtEnd = true;
                    View.InvalidateVisual();
                }));
            }) { IsBackground = true, Name = "uninstall" };
            worker.Start();
        }

        // ------------------------------------------------------------------ closing

        void OnClosing(object sender, System.ComponentModel.CancelEventArgs e)
        {
            if (closeNow) return;
            if (M.Installing)
            {
                e.Cancel = true;
                if (M.Ask == null) Back();
                return;
            }
            if (M.Removing == 1) { e.Cancel = true; return; }
            if (closing) return;
            closing = true;
            if (Music.Playing)
            {
                // The music fades out over 0.3 s before the window goes.
                e.Cancel = true;
                ThreadPool.QueueUserWorkItem(_ =>
                {
                    Music.StopNow(0.3);
                    Dispatcher.BeginInvoke(new Action(() => { closeNow = true; Close(); }));
                });
            }
        }
    }
}
