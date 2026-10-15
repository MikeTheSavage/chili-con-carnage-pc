// The window: hosts the picture, feeds it the keyboard and the gamepad, opens the system's file
// pickers, and starts the game (hiding while it runs, coming back when it ends).
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;
using System.Windows;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;

namespace ChiliLauncher
{
    public sealed class GameRun
    {
        public Process Process;
        public DateTime Started;
        public string LogFile;
        public StreamWriter Out, Err;
    }

    /// <summary>The game could not be started: a short sentence for the message line, and what there is to add behind Details.</summary>
    public sealed class StartFailed : Exception
    {
        public readonly string Detail;
        public StartFailed(string sentence, string detail = null) : base(sentence) { Detail = detail; }
    }

    /// <summary>Starting the game program with the launcher's settings in force.</summary>
    public static class GameStarter
    {
        // Switches of scripted test runs: a start by a player has none of them, and the settings file decides.
        static readonly string[] clear =
        {
            "CHILI_DETERMINISTIC", "CHILI_INPUT_REPLAY", "CHILI_INPUT_RECORD", "CHILI_AUTO_DIALOGS", "CHILI_NULL_AUDIO", "CHILI_UNPACED",
            "CHILI_MS0_DIR", "CHILI_UNATTENDED", "CHILI_DISC_ANSWER", "CHILI_CRASH_TEST", "PSPRECOMP_PROBE_SECONDS", "PSPRECOMP_LIVE_CONTROL",
            "CHILI_RENDER_BACKEND", "CHILI_RENDER_SCALE", "CHILI_RENDER_MSAA", "CHILI_RENDER_AA", "CHILI_RENDER_ANISOTROPY", "CHILI_RENDER_FILTER",
            "CHILI_RENDER_SSAO", "CHILI_RENDER_GLOW", "CHILI_RENDER_PRESET", "CHILI_RENDER_EARLY_PRESENT", "CHILI_RENDER_GE_THREAD", "CHILI_RENDER_STRICT",
            "CHILI_AUDIO_BUFFER_MS", "CHILI_GAME_PRIORITY", "CHILI_MODS_DIR", "CHILI_TEXTURE_DUMP", "CHILI_CONFIG",
        };

        // The scripted check of a packaged start (scripts\check_package_start.ps1) sets CHILI_LAUNCHER_CHECK=1:
        // the game then keeps the three switches that make it a run nobody watches (its window behind the
        // others and never in front, no sound, an end after some seconds). The settings file is still the
        // launcher's, and every other switch is still taken away.
        public static bool ScriptedCheck { get { return Environment.GetEnvironmentVariable("CHILI_LAUNCHER_CHECK") == "1"; } }
        static readonly string[] keptForTheCheck = { "CHILI_UNATTENDED", "CHILI_NULL_AUDIO", "PSPRECOMP_PROBE_SECONDS" };

        public static string Expand(Model m, string arguments)
        {
            return arguments.Replace("{disc}", m.Disc.Path).Replace("{boot}", m.Disc.Boot).Replace("{saves}", m.SavesDir).Replace("{ini}", m.IniPath);
        }

        /// <summary>Starts the game. Throws StartFailed with a sentence for the player when it cannot.</summary>
        public static GameRun Start(Model m)
        {
            string exe = m.Game.ExePath;
            if (!File.Exists(exe)) throw new StartFailed("The game program is missing.", exe);
            string arguments = m.Config.Get("Arguments", "");
            bool developer = arguments.Length > 0;
            if (developer && arguments.Contains("{boot}") && m.Disc.IsImage)
                throw new StartFailed("This game build needs the unpacked disc folder, not an .iso.");
            var info = new ProcessStartInfo(exe, developer ? Expand(m, arguments) : "")
            {
                UseShellExecute = false,
                WorkingDirectory = Path.GetDirectoryName(exe),
            };
            bool check = ScriptedCheck;
            foreach (var name in clear) if (!check || Array.IndexOf(keptForTheCheck, name) < 0) info.EnvironmentVariables.Remove(name);
            info.EnvironmentVariables.Remove("CHILI_LAUNCHER_CHECK");
            // The game reads ChiliNative.ini beside itself; when the launcher's file is elsewhere, name it.
            if (!string.Equals(Path.GetFullPath(m.IniDir).TrimEnd('\\'), Path.GetFullPath(Path.GetDirectoryName(exe)).TrimEnd('\\'), StringComparison.OrdinalIgnoreCase))
                info.EnvironmentVariables["CHILI_CONFIG"] = m.IniPath;
            var run = new GameRun { Started = DateTime.Now };
            if (developer)
            {
                // A developer's start prints its log instead of writing one: keep it, as the old starter did.
                Directory.CreateDirectory(m.LogsDir);
                Directory.CreateDirectory(m.SavesDir);
                info.RedirectStandardOutput = true;
                info.RedirectStandardError = true;
                info.CreateNoWindow = true;
                run.LogFile = Path.Combine(m.LogsDir, "last_run.log");
                run.Out = new StreamWriter(new FileStream(run.LogFile, FileMode.Create, FileAccess.Write, FileShare.Read), new UTF8Encoding(false)) { AutoFlush = true };
                run.Err = new StreamWriter(new FileStream(Path.Combine(m.LogsDir, "last_run.err.log"), FileMode.Create, FileAccess.Write, FileShare.Read), new UTF8Encoding(false)) { AutoFlush = true };
            }
            var p = new Process { StartInfo = info, EnableRaisingEvents = true };
            if (developer)
            {
                p.OutputDataReceived += (s, e) => { if (e.Data != null) lock (run) { try { run.Out.WriteLine(e.Data); } catch { } } };
                p.ErrorDataReceived += (s, e) => { if (e.Data != null) lock (run) { try { run.Err.WriteLine(e.Data); } catch { } } };
            }
            try { p.Start(); }
            catch (Exception e)
            {
                if (run.Out != null) run.Out.Dispose();
                if (run.Err != null) run.Err.Dispose();
                throw new StartFailed("Windows could not start the game.", exe + "\n\n" + e.Message.TrimEnd('.') + ".");
            }
            if (developer) { p.BeginOutputReadLine(); p.BeginErrorReadLine(); }
            run.Process = p;
            return run;
        }

        /// <summary>The last lines the game wrote, without the noise.</summary>
        public static string Tail(Model m, GameRun run, int lines)
        {
            try
            {
                var candidates = new List<string>();
                if (run != null && run.LogFile != null)
                {
                    candidates.Add(Path.Combine(Path.GetDirectoryName(run.LogFile), "last_run.err.log"));
                    candidates.Add(run.LogFile);
                }
                string latest = m.LatestLog();
                if (latest != null) candidates.Add(latest);
                foreach (var file in candidates)
                {
                    if (!File.Exists(file)) continue;
                    if (run != null && File.GetLastWriteTime(file) < run.Started.AddSeconds(-2)) continue;
                    string text;
                    using (var f = new FileStream(file, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
                    {
                        long take = Math.Min(f.Length, 16384);
                        f.Seek(-take, SeekOrigin.End);
                        var buffer = new byte[take];
                        int got = f.Read(buffer, 0, buffer.Length);
                        text = Encoding.UTF8.GetString(buffer, 0, got);
                    }
                    var all = text.Split('\n').Select(l => l.Trim()).Where(l => l.Length > 0 && l.IndexOf("swscaler", StringComparison.OrdinalIgnoreCase) < 0).ToList();
                    if (all.Count == 0) continue;
                    return string.Join("\n", all.Skip(Math.Max(0, all.Count - lines)));
                }
            }
            catch { }
            return "";
        }
    }

    public sealed class MainWindow : Window
    {
        readonly Model model;
        readonly Surface ui;
        readonly DispatcherTimer padTimer;
        readonly Stopwatch clock = Stopwatch.StartNew();
        GameRun run;
        bool directStart;
        /// <summary>Tests: the window does not take the focus, neither when it opens nor when it comes back.</summary>
        public bool Quiet;
        /// <summary>Tests: the controller's state comes from the test, not from XInput.</summary>
        public bool FakePad;
        /// <summary>Tests: act as if the launcher were (or were not) the window in front.</summary>
        public bool? FrontOverride;
        IntPtr handle;
        // PlayStation controllers arrive as Raw Input at the window; one controller is listened to at a time.
        readonly HidPads hid = new HidPads();
        readonly PadArbiter arbiter = new PadArbiter();
        readonly DispatcherTimer rumbleStop;
        int rumbling = -1;

        /// <summary>True while the launcher is the window the player is using.</summary>
        bool InFront()
        {
            if (FrontOverride.HasValue) return FrontOverride.Value;
            try { return handle != IntPtr.Zero && Win32.GetForegroundWindow() == handle; }
            catch { return IsActive; }
        }
        public bool GameRunning { get { return run != null; } }

        public Surface Surface { get { return ui; } }

        public MainWindow(Model m, ArtFolder art, string startPage, bool direct)
        {
            model = m;
            directStart = direct;
            Title = "Chili Con Carnage";       // the versions follow it once the picture exists (UpdateTitle)
            Background = new SolidColorBrush(Pal.RustDark);
            UseLayoutRounding = true;
            try
            {
                var stream = typeof(MainWindow).Assembly.GetManifestResourceStream("icon");
                if (stream != null) Icon = BitmapFrame.Create(stream, BitmapCreateOptions.None, BitmapCacheOption.OnLoad);
            }
            catch { }
            ui = new Surface(m, art);
            Content = ui;
            UpdateTitle();
            // 1280x720 of picture, less when the desktop is small.
            var area = SystemParameters.WorkArea;
            double w = 1280, h = 720;
            if (area.Width < 1320 || area.Height < 800) { w = 960; h = 540; }
            ui.Width = w; ui.Height = h;
            SizeToContent = SizeToContent.WidthAndHeight;
            WindowStartupLocation = WindowStartupLocation.CenterScreen;
            ui.PlayRequested = Play;
            ui.ChooseDiscRequested = ChooseDisc;
            ui.OpenPathRequested = OpenPath;
            ui.RescanRequested = Rescan;
            if (!string.IsNullOrEmpty(startPage)) ui.ShowPage(startPage);

            padTimer = new DispatcherTimer(DispatcherPriority.Input) { Interval = TimeSpan.FromMilliseconds(16) };
            padTimer.Tick += (s, e) =>
            {
                ui.Tick();
                // While another window is in front nothing in the launcher moves by itself (the flames of the
                // HD pack choice) and the controller is not listened to.
                bool front = InFront();
                if (front != ui.WindowActive) { ui.WindowActive = front; ui.InvalidateVisual(); }
                if (!FakePad)
                {
                    double now = clock.Elapsed.TotalSeconds;
                    // The launcher reads PlayStation pads for its own use whatever "Use PlayStation controllers"
                    // says: that switch is the game's key, and a player whose only controller is one must be
                    // able to switch it back on with it.
                    ui.PadUpdate(Listen(XInput.Read(now), hid.Read(now, front), now), front);
                }
            };
            // The Rumble row's Test: the connected Xbox controller shakes for a third of a second.
            rumbleStop = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(330) };
            rumbleStop.Tick += (s, e) => StopRumble();
            ui.RumbleRequested = strength =>
            {
                var pad = ui.Pad;
                if (FakePad || !pad.Connected || pad.Kind != PadKind.Xbox) return;
                StopRumble();
                if (strength > 0 && XInput.Vibrate(pad.Index, strength)) { rumbling = pad.Index; rumbleStop.Start(); }
            };

            SourceInitialized += (s, e) =>
            {
                handle = new WindowInteropHelper(this).Handle;
                Win32.DarkTitleBar(handle);
                var source = HwndSource.FromHwnd(handle);
                if (source != null) source.AddHook(WindowMessage);
                hid.Attach(handle);
            };
            Closed += (s, e) => StopRumble();
            ContentRendered += (s, e) =>
            {
                if (SizeToContent == SizeToContent.Manual) return;
                // The window keeps the size it has now and can be resized; the picture follows it.
                double chromeW = ActualWidth - ui.ActualWidth, chromeH = ActualHeight - ui.ActualHeight;
                SizeToContent = SizeToContent.Manual;
                ui.Width = double.NaN; ui.Height = double.NaN;
                MinWidth = 960 + chromeW; MinHeight = 540 + chromeH;
                Dispatcher.BeginInvoke(DispatcherPriority.Background, new Action(ProbeGame));
            };
            Loaded += (s, e) => { padTimer.Start(); if (!Quiet) ui.Focus(); };
            Activated += (s, e) => { if (run == null && IsLoaded && !Quiet) Refresh(); };
            Closing += (s, e) =>
            {
                // Closing saves, as PLAY does. If the file cannot be written the window stays, once, to say so.
                if (run != null) return;
                if (!ui.Save() && !closeAnyway) { closeAnyway = true; e.Cancel = true; }
            };
        }
        bool closeAnyway;

        /// <summary>
        /// The controller the launcher listens to now, of the Xbox kind's state and the PlayStation pad's. No
        /// setting is asked: "Use PlayStation controllers" is the game's key and does not switch the
        /// launcher's own listening off.
        /// </summary>
        public PadState Listen(PadState xbox, PadState playStation, double now) { return arbiter.Choose(xbox, playStation, now); }

        /// <summary>The title bar: "Chili Con Carnage", then the launcher's and the game's version.</summary>
        void UpdateTitle() { Title = ui.WindowTitle(); }

        void StopRumble()
        {
            rumbleStop.Stop();
            if (rumbling >= 0) { XInput.Vibrate(rumbling, 0); rumbling = -1; }
        }

        /// <summary>Raw Input: a PlayStation controller's report, or a controller that came or went.</summary>
        IntPtr WindowMessage(IntPtr hwnd, int message, IntPtr wParam, IntPtr lParam, ref bool handled)
        {
            if (message == HidPads.WmInput) hid.Input(lParam);
            else if (message == HidPads.WmInputDeviceChange) hid.Scan();
            return IntPtr.Zero;
        }

        void ProbeGame()
        {
            // Which settings does the game beside us understand? Reading 100 MB takes a moment the first time.
            var worker = new System.ComponentModel.BackgroundWorker();
            GameInfo info = null;
            worker.DoWork += (s, e) => info = GameInfo.Inspect(GameInfo.Find(model.BaseDir, model.Config), model.Config);
            worker.RunWorkerCompleted += (s, e) => { if (info != null) { model.Game = info; ui.Rebuild(); UpdateTitle(); } };
            worker.RunWorkerAsync();
        }

        void Refresh()
        {
            model.ScanMods();
            model.RefreshDisc();
            // A game program that was missing is looked for again whenever the player comes back to the window.
            if (!model.Game.Exists) ProbeGame();
            ui.Rebuild();
        }

        void Rescan()
        {
            XInput.Rescan();
            hid.Rescan();
            if (!model.AnyChanged) model.Reload(); else { model.ScanMods(); model.RefreshDisc(); }
            ProbeGame();
            // Nothing is said: the page shows what was found, and a standing error keeps its place and its button.
            ui.Rebuild();
        }

        protected override void OnPreviewKeyDown(KeyEventArgs e)
        {
            Key key = e.Key == Key.System ? e.SystemKey : e.Key == Key.ImeProcessed ? e.ImeProcessedKey : e.Key;
            var mods = Keyboard.Modifiers;
            if (key == Key.F4 && (mods & ModifierKeys.Alt) != 0) return;   // Alt+F4 always closes
            if (ui.HandleKey(key, mods, e.IsRepeat)) e.Handled = true;
            base.OnPreviewKeyDown(e);
        }

        void OpenPath(string path)
        {
            try
            {
                // A folder, a file, or a page of the Windows settings (the volume mixer of the Sound page).
                if (path.StartsWith("ms-settings:", StringComparison.OrdinalIgnoreCase) || Directory.Exists(path) || File.Exists(path))
                    Process.Start(new ProcessStartInfo(path) { UseShellExecute = true });
                else ui.SayProblem("Not there yet.", "Not there yet", path);
            }
            catch (Exception e) { ui.SayProblem("Windows could not open this.", "Could not open", path + "\n\n" + e.Message.TrimEnd('.') + "."); }
        }

        void ChooseDisc(bool iso)
        {
            string picked = null;
            if (iso)
            {
                var dialog = new Microsoft.Win32.OpenFileDialog
                {
                    Title = "Choose the disc image",
                    Filter = "Disc image (*.iso)|*.iso|All files (*.*)|*.*",
                    CheckFileExists = true
                };
                if (dialog.ShowDialog(this) == true) picked = dialog.FileName;
            }
            else picked = FolderPicker.Show(new WindowInteropHelper(this).Handle, "Choose the disc folder");
            if (picked == null) return;
            var result = model.ChooseDisc(picked);
            if (result.Ok) { ui.Say("Disc found: " + result.Path, Pal.ChalkGreen, 4); }
            else { model.Disc = result; ui.SayError(result.Message); }
            ui.Rebuild();
        }

        /// <summary>PLAY: write the settings, start the game, step aside until it ends.</summary>
        public void Play()
        {
            if (run != null || !ui.CanPlay) return;
            if (!ui.Save()) { if (!IsVisible) Show(); return; }
            try { run = GameStarter.Start(model); }
            catch (Exception e)
            {
                ui.LastError = "";
                var failed = e as StartFailed;
                ui.SayProblem(failed != null ? failed.Message : "The game could not be started.", "Could not start", failed != null ? failed.Detail : e.Message);
                if (!IsVisible) Show();
                return;
            }
            ui.LastError = "";
            padTimer.Stop();
            StopRumble();
            hid.Rest();
            // The timer that says whether the launcher is in front is stopped: without this the fire of the HD
            // pack choice would keep the hidden window drawing for the whole game session.
            ui.WindowActive = false;
            ui.Hidden = true;
            Hide();
            var started = run;
            started.Process.Exited += (s, e) => Dispatcher.BeginInvoke(new Action(() => GameEnded(started)));
            if (started.Process.HasExited) Dispatcher.BeginInvoke(new Action(() => GameEnded(started)));
        }

        void GameEnded(GameRun ended)
        {
            if (run != ended) return;
            run = null;
            int code = 0;
            double seconds = (DateTime.Now - ended.Started).TotalSeconds;
            try { ended.Process.WaitForExit(2000); code = ended.Process.ExitCode; } catch { }
            lock (ended) { try { if (ended.Out != null) ended.Out.Dispose(); if (ended.Err != null) ended.Err.Dispose(); } catch { } }
            bool failed = code != 0;
            if (!failed && (directStart || model.Config.Get("AfterGame", "return").Trim().ToLowerInvariant() == "close")) { Close(); return; }
            directStart = false;
            model.Reload();
            ui.ShowPage("Play");
            if (failed)
            {
                ui.LastError = GameStarter.Tail(model, ended, 4);
                // One sentence; where the log is, is the button's business.
                ui.SayError(seconds < 3 ? StoppedAtOnce : "The game stopped with error " + code + ".", "Open logs", () => OpenPath(LogDir()));
            }
            ui.Hidden = false;
            ui.Rebuild();
            // Nothing held is carried across the game: a button that is down when the launcher comes back
            // (the one that left the game) is not a press here, and what a PlayStation pad held when PLAY
            // was pressed is forgotten.
            hid.Rest();
            ui.WaitForPadRelease();
            UpdateTitle();
            padTimer.Start();
            Show();
            if (WindowState == WindowState.Minimized) WindowState = WindowState.Normal;
            if (!Quiet) Activate();
        }

        string LogDir()
        {
            string log = model.LatestLog();
            return log != null ? Path.GetDirectoryName(log) : model.LogsDir;
        }
        public const string StoppedAtOnce = "The game stopped right away.";
    }
}
