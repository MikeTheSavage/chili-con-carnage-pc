// Entry point. Without arguments: the launcher window (or, when the player asked for it, the game at
// once). --screenshot renders every page to PNG files without a window; --test-ini checks the reader
// and writer of the settings file.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Threading;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;

[assembly: AssemblyTitle("Chili Con Carnage launcher")]
[assembly: AssemblyProduct("Chili Con Carnage PC")]
[assembly: AssemblyDescription("Settings and start for the native PC build of Chili Con Carnage")]
// The version, the publisher and the notice come from launcher\build.ps1 (the game program's own).
[assembly: System.Runtime.Versioning.TargetFramework(".NETFramework,Version=v4.8", FrameworkDisplayName = ".NET Framework 4.8")]

namespace ChiliLauncher
{
    public static class Program
    {
        const string Usage =
            "Chili Con Carnage launcher\n" +
            "  (no arguments)              the settings window; PLAY starts the game beside it\n" +
            "  --settings                  show the window even when 'skip this window' is on (so does holding Shift)\n" +
            "  --ini <folder or file>      where ChiliNative.ini is (default: the launcher's folder)\n" +
            "  --page <name>               open on a page: Play, Display, Graphics, Controls, \"Textures & mods\", Sound, \"Game data\"\n" +
            "  --no-art                    ignore the art folder\n" +
            "  --base <folder>             act as if the launcher were in this folder (for tests)\n" +
            "  --screenshot <folder> [--size 1280x720] [--scale 1] [--only <start of a name>] [--fire-time <seconds>]\n" +
            "                              render every page to PNG files and exit.\n" +
            "                              --size is the window in units (what the layout sees), --scale the monitor's\n" +
            "                              scale: the pictures are size times scale pixels (1280x720 at 2 = 2560x1440)\n" +
            "  --test-ini [file]           test the settings reader and writer (on a copy of the file) and exit\n" +
            "  --selftest <folder>         drive the real window through keys, clicks and PLAY (use with --base on a test folder)\n" +
            "  --pads                      list the controllers as the launcher sees them (XInput slots, Raw Input devices) and exit\n" +
            "  --every-value <folder> [--disc <.iso or folder>]...\n" +
            "                              for the check that launcher and game agree: every row put to each of its values\n" +
            "                              through its own control, off screen, and the settings file of each kept in the\n" +
            "                              folder (which holds the ChiliNative.ini to start from); use with --base\n";

        [STAThread]
        public static int Main(string[] args)
        {
            string ini = null, shots = null, page = null, testIni = null, baseOverride = null, selfTest = null, only = null, measure = null, everyValue = null;
            var discs = new List<string>();
            bool settings = false, noArt = false, test = false, measureBack = false, pads = false, measureWhole = false;
            int measureStick = 0;
            double measureSeconds = 10;
            int w = 1280, h = 720;
            double scale = 1;
            for (int i = 0; i < args.Length; i++)
            {
                string a = args[i];
                Func<string> next = () => i + 1 < args.Length ? args[++i] : "";
                switch (a.ToLowerInvariant())
                {
                    case "--ini": ini = next(); break;
                    case "--base": baseOverride = next(); break;
                    case "--settings": settings = true; break;
                    case "--page": page = next(); break;
                    case "--no-art": noArt = true; break;
                    case "--screenshot": shots = next(); break;
                    case "--only": only = next(); break;
                    case "--fire-time": { double ft; if (double.TryParse(next(), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out ft)) Surface.StaticFireTime = ft; break; }
                    case "--selftest": selfTest = next(); break;
                    case "--measure": measure = next(); break;
                    case "--seconds": double.TryParse(next(), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out measureSeconds); break;
                    case "--back": measureBack = true; break;
                    case "--stick": measureStick = 1; break;
                    case "--stick-out-of-view": measureStick = 2; break;
                    case "--whole": measureWhole = true; break;
                    case "--pads": pads = true; break;
                    case "--every-value": everyValue = next(); break;
                    case "--disc": discs.Add(next()); break;
                    case "--size":
                        {
                            var parts = next().ToLowerInvariant().Split('x');
                            if (parts.Length == 2) { int.TryParse(parts[0], out w); int.TryParse(parts[1], out h); }
                            break;
                        }
                    case "--scale": double.TryParse(next(), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out scale); break;
                    case "--test-ini": test = true; if (i + 1 < args.Length && !args[i + 1].StartsWith("--")) testIni = next(); break;
                    case "--help": case "-h": case "/?":
                        Win32.AttachConsole(-1);
                        Console.WriteLine(Usage);
                        return 0;
                }
            }
            string baseDir = AppDomain.CurrentDomain.BaseDirectory.TrimEnd('\\');
            if (!string.IsNullOrEmpty(baseOverride)) baseDir = Path.GetFullPath(baseOverride).TrimEnd('\\');
            if (test) { Win32.AttachConsole(-1); return IniTests.Run(testIni); }
            if (pads)
            {
                // For a player whose controller does nothing: what Windows reports, and what the launcher makes of it.
                Win32.AttachConsole(-1);
                Console.WriteLine("Xbox kind (XInput):");
                for (int slot = 0; slot < 4; slot++) Console.WriteLine("  slot " + (slot + 1) + ": " + (XInput.Connected(slot) ? "a controller" : "empty"));
                var hid = new HidPads();
                hid.Scan();
                Console.WriteLine("Raw Input, HID devices (PlayStation controllers are read from here):");
                foreach (var line in hid.Seen) Console.WriteLine("  " + line);
                if (hid.Seen.Count == 0) Console.WriteLine("  none");
                return 0;
            }
            if (everyValue != null)
            {
                Win32.AttachConsole(-1);
                try { return EveryValue.Run(baseDir, everyValue, discs); }
                catch (Exception e) { Console.Error.WriteLine("every-value failed: " + e); return 1; }
            }
            if (shots != null)
            {
                Win32.AttachConsole(-1);
                try { return Screenshots.Run(baseDir, ini, shots, Math.Max(640, w), Math.Max(360, h), Math.Max(0.5, Math.Min(4, scale)), !noArt, only); }
                catch (Exception e) { Console.Error.WriteLine("screenshot failed: " + e); return 1; }
            }

            if (selfTest != null)
            {
                // The self test changes settings and presses PLAY: only on a made-up game folder, never on a real one.
                if (!File.Exists(Path.Combine(baseDir, "made_up_game_folder.txt")))
                {
                    Win32.AttachConsole(-1);
                    Console.Error.WriteLine("--selftest runs only on the made-up game folder of shots.ps1 (--base bin\\fixture): " + baseDir + " is not one.");
                    return 2;
                }
                var testModel = new Model(baseDir, ini);
                var testApp = new Application { ShutdownMode = ShutdownMode.OnMainWindowClose };
                var testWindow = new MainWindow(testModel, new ArtFolder(baseDir, !noArt), null, false);
                testApp.MainWindow = testWindow;
                return SelfTest.Run(testApp, testWindow, testModel, selfTest);
            }

            if (measure != null)
            {
                // Processor time of the live window over some seconds, on the made-up game folder only (closing saves).
                if (!File.Exists(Path.Combine(baseDir, "made_up_game_folder.txt"))) return 2;
                var measured = new Model(baseDir, ini);
                var measureApp = new Application { ShutdownMode = ShutdownMode.OnMainWindowClose };
                var measureWindow = new MainWindow(measured, new ArtFolder(baseDir, !noArt), page, false);
                measureApp.MainWindow = measureWindow;
                return Measure.Run(measureApp, measureWindow, measure, Math.Max(1, Math.Min(60, measureSeconds)), !measureBack, measureStick, measureWhole);
            }

            // One launcher at a time: a second start shows the first one.
            bool created;
            var signal = new EventWaitHandle(false, EventResetMode.AutoReset, "ChiliConCarnageLauncher.Show", out created);
            if (!created) { signal.Set(); return 0; }

            var model = new Model(baseDir, ini);
            // A file of an older game build: brought up to date as the game would at its next start.
            model.MigrateInput();
            bool direct = model.Config.GetBool("StartGameDirectly", false) && !settings && !Win32.ShiftHeld() && page == null;
            var app = new Application { ShutdownMode = ShutdownMode.OnMainWindowClose };
            var window = new MainWindow(model, new ArtFolder(baseDir, !noArt), page, direct);
            app.MainWindow = window;
            // The scripted check of a packaged start: should the window have to appear (the game failed), it
            // does not take the focus from whoever is at the PC.
            if (GameStarter.ScriptedCheck) { window.ShowActivated = false; window.Quiet = true; }
            ThreadPool.RegisterWaitForSingleObject(signal, (state, timedOut) => window.Dispatcher.BeginInvoke(new Action(() =>
            {
                if (!window.IsVisible) return;      // the game is running
                if (window.WindowState == WindowState.Minimized) window.WindowState = WindowState.Normal;
                window.Activate();
            })), null, Timeout.Infinite, false);
            // A fault in the launcher is written down and shown in the message line; the window stays.
            app.DispatcherUnhandledException += (s, e) =>
            {
                try { File.AppendAllText(Path.Combine(baseDir, "launcher_error.log"), DateTime.Now.ToString("s") + " " + e.Exception + Environment.NewLine); } catch { }
                try { window.Surface.SayProblem("Something went wrong in the launcher.", "Launcher error", e.Exception.Message); } catch { }
                e.Handled = true;
            };
            if (direct && window.Surface.CanPlay)
            {
                // The player asked to skip the window: start the game now. The window appears only if that fails.
                app.Startup += (s, e) => window.Play();
                return app.Run();
            }
            return app.Run(window);
        }
    }

    /// <summary>
    /// --measure <file> [--seconds 10] [--page <name>] [--back] [--stick | --stick-out-of-view] [--whole]: the
    /// live window, shown without taking the focus, left alone for some seconds; the processor time the
    /// launcher used meanwhile and how often it drew are appended to the file. --back: as when another
    /// window is in front. --stick: the Gamepad tab with the stick dials in view and a made-up controller
    /// whose sticks move all the time, as a stick in the hand does (--stick-out-of-view: the same with the
    /// dials scrolled away). --whole: every stick sample draws the whole picture, as the launcher did before
    /// the dots had a visual of their own (for the comparison).
    /// </summary>
    public static class Measure
    {
        public static int Run(Application app, MainWindow window, string file, double seconds, bool front, int stick = 0, bool whole = false)
        {
            window.ShowActivated = false;
            window.Quiet = true;
            window.FrontOverride = front;
            var ui = window.Surface;
            ui.IgnoreMouse = true;
            System.Windows.Threading.DispatcherTimer hand = null;
            if (stick != 0)
            {
                // A stick held by a hand: it circles slowly and never reports the same value twice.
                window.FakePad = true;
                var noise = new Random(7);
                var held = System.Diagnostics.Stopwatch.StartNew();
                hand = new System.Windows.Threading.DispatcherTimer(System.Windows.Threading.DispatcherPriority.Input) { Interval = TimeSpan.FromMilliseconds(16) };
                hand.Tick += (s, e) =>
                {
                    double t = held.Elapsed.TotalSeconds;
                    ui.PadUpdate(new PadState
                    {
                        Connected = true, Kind = PadKind.Xbox,
                        LX = (short)(12000 * Math.Sin(t * 3.0) + noise.Next(-60, 61)), LY = (short)(12000 * Math.Cos(t * 3.0) + noise.Next(-60, 61)),
                        RX = (short)(30000 * Math.Sin(t * 2.1) + noise.Next(-60, 61)), RY = (short)(8000 * Math.Cos(t * 2.1) + noise.Next(-60, 61)),
                    }, true);
                    if (whole) ui.InvalidateVisual();
                };
            }
            var process = System.Diagnostics.Process.GetCurrentProcess();
            var timer = new System.Windows.Threading.DispatcherTimer(System.Windows.Threading.DispatcherPriority.Background);
            int phase = 0;
            TimeSpan cpu = TimeSpan.Zero;
            int renders = 0, parts = 0, live = 0;
            var threads = new Dictionary<int, TimeSpan>();
            int uiThread = Native.CurrentThreadId();
            var wall = new System.Diagnostics.Stopwatch();
            timer.Tick += (s, e) =>
            {
                timer.Stop();
                process.Refresh();
                if (phase == 0)
                {
                    phase = 1;
                    cpu = process.TotalProcessorTime; renders = ui.Renders; parts = ui.PartRenders; live = ui.LiveRenders;
                    foreach (System.Diagnostics.ProcessThread t in process.Threads) { try { threads[t.Id] = t.TotalProcessorTime; } catch { } }
                    wall.Restart();
                    timer.Interval = TimeSpan.FromSeconds(seconds);
                    timer.Start();
                    return;
                }
                double used = (process.TotalProcessorTime - cpu).TotalSeconds, took = wall.Elapsed.TotalSeconds;
                string line = string.Format(System.Globalization.CultureInfo.InvariantCulture,
                    "{0:yyyy-MM-dd HH:mm:ss}  page={1}{9}  front={2}  {3:0.0} s  processor time {4:0.000} s = {5:0.0} % of one core  whole picture drawn {6} times  fire alone drawn {7} times  stick dots or waiting chip alone drawn {10} times  something moves by itself: {8}",
                    DateTime.Now, ui.PageName, front, took, used, 100 * used / took, ui.Renders - renders, ui.PartRenders - parts, ui.Animating,
                    stick == 0 ? "" : (stick == 1 ? ", Gamepad tab, stick dials in view, a made-up stick moving" : ", Gamepad tab, stick dials out of view, a made-up stick moving") + (whole ? ", every sample drawing the whole picture (the old way)" : ""),
                    ui.LiveRenders - live);
                // Which threads the time went to: the launcher's own (it draws) and the ones of Windows' compositor for WPF.
                var busy = new List<KeyValuePair<double, string>>();
                foreach (System.Diagnostics.ProcessThread t in process.Threads)
                {
                    try
                    {
                        TimeSpan was;
                        double spent = (t.TotalProcessorTime - (threads.TryGetValue(t.Id, out was) ? was : TimeSpan.Zero)).TotalSeconds;
                        if (spent >= 0.01) busy.Add(new KeyValuePair<double, string>(spent, (t.Id == uiThread ? "the launcher's own thread " : "thread " + t.Id + " ") + spent.ToString("0.000", System.Globalization.CultureInfo.InvariantCulture) + " s"));
                    }
                    catch { }
                }
                busy.Sort((a, b) => b.Key.CompareTo(a.Key));
                if (busy.Count > 0) line += "  (" + string.Join(", ", busy.Select(b => b.Value).Take(4)) + ")";
                line += "  render tier " + (System.Windows.Media.RenderCapability.Tier >> 16);
                try { File.AppendAllText(file, line + Environment.NewLine); } catch { }
                window.Close();
            };
            window.ContentRendered += (s, e) =>
            {
                if (phase != 0 || timer.IsEnabled) return;
                if (stick != 0)
                {
                    ui.ShowPage("Controls");
                    ui.SetPadView(true);
                    if (stick == 1) ui.ScrollToRow("group:Sticks and triggers");
                    hand.Start();
                }
                timer.Interval = TimeSpan.FromSeconds(3);
                timer.Start();
            };
            app.Run(window);
            return 0;
        }
    }

    /// <summary>Renders every page, and the states worth checking, to PNG files without showing a window.</summary>
    public static class Screenshots
    {
        static void Arrange(Surface ui, int w, int h)
        {
            ui.InvalidateVisual();
            ui.Measure(new Size(w, h));
            ui.Arrange(new Rect(0, 0, w, h));
            ui.UpdateLayout();
        }

        public static int Run(string baseDir, string ini, string outDir, int w, int h, double scale, bool useArt, string only = null)
        {
            Directory.CreateDirectory(outDir);
            Tx.PixelsPerDip = scale;
            int count = 0;
            // Every text each picture draws, for reading through: texts.txt beside the pictures.
            var texts = new System.Text.StringBuilder();
            var padKind = PadKind.Xbox;
            ushort padPid = 0;
            Action<string, Action<Model>, Action<Surface>, bool> shot = (name, prepare, act, pad) =>
            {
                if (!string.IsNullOrEmpty(only) && !name.StartsWith(only, StringComparison.OrdinalIgnoreCase)) return;
                var m = new Model(baseDir, ini);
                // As the launcher starts for a player: a settings file of an older game build brought up to date.
                m.MigrateInput();
                m.InspectGame();
                m.Reload();
                if (prepare != null) prepare(m);
                var ui = new Surface(m, new ArtFolder(baseDir, useArt)) { Static = true };
                if (pad) ui.Pad = new PadState { Connected = true, Kind = padKind, Pid = padPid, Index = 0, LX = 9000, LY = 14000, RX = -3000, RY = -2000 };
                Arrange(ui, w, h);
                if (act != null) act(ui);
                Tx.Log = new List<string>();
                Arrange(ui, w, h);
                var drawn = Tx.Log;
                Tx.Log = null;
                texts.AppendLine(name);
                foreach (var t in drawn.Distinct()) texts.AppendLine("    " + t.Replace("\n", " / "));
                var bitmap = new RenderTargetBitmap((int)Math.Round(w * scale), (int)Math.Round(h * scale), 96 * scale, 96 * scale, PixelFormats.Pbgra32);
                bitmap.Render(ui);
                var encoder = new PngBitmapEncoder();
                encoder.Frames.Add(BitmapFrame.Create(bitmap));
                string file = Path.Combine(outDir, name + ".png");
                using (var f = File.Create(file)) encoder.Save(f);
                Console.WriteLine(file);
                count++;
            };

            shot("01_play", null, ui => ui.ShowPage("Play"), false);
            // The disc as an image file at a short path, as a player's own copy usually is.
            shot("01b_play_disc_image", m => m.Disc = new DiscInfo { Path = @"D:\Games\Chili Con Carnage (USA).iso", State = DiscState.Found, IsImage = true }, ui => ui.ShowPage("Play"), false);
            shot("02_play_game_data_not_found", m => m.Disc = new DiscInfo(), ui => { ui.ShowPage("Play"); ui.FocusRow("choose", 0); }, false);
            shot("03_play_wrong_disc", m => m.Disc = new DiscInfo { Path = @"D:\Games\Another Game.iso", State = DiscState.WrongDisc, Message = DiscCheck.WrongText, IsImage = true },
                ui => ui.ShowPage("Play"), false);
            shot("04_play_game_failed", null, ui =>
            {
                ui.ShowPage("Play");
                ui.LastError = "[start] Chili Con Carnage PC 0.9, folder C:\\Games\\ChiliConCarnagePC\n[render] Direct3D 12 could not start: no graphics card with feature level 11_0\n[stop] the renderer could not be started";
                ui.Rebuild();
                ui.SayError(MainWindow.StoppedAtOnce, "Open logs", () => { });
            }, false);
            shot("05_display", null, ui => ui.ShowPage("Display"), false);
            shot("05b_display_resolution_3x", m => m.Set(S.InternalScale, "3"), ui => ui.ShowPage("Display"), false);
            shot("06_display_focused_row", null, ui => { ui.ShowPage("Display"); ui.FocusRow(S.InternalScale.Id); }, false);
            shot("07_display_changed_unsaved", m => { m.Set(S.Fullscreen, "true"); m.Set(S.FrameLock, "2"); }, ui => ui.ShowPage("Display"), false);
            shot("07b_display_advanced_auto_limit", m => m.Set(S.Preset, "medium"), ui => { ui.ShowPage("Display"); ui.OpenAdvanced("Display"); ui.FocusRow(S.Preset.Id); }, false);
            shot("08_graphics", null, ui => ui.ShowPage("Graphics"), false);
            shot("08b_graphics_custom", m => m.Set(S.Anisotropy, "8"), ui => ui.ShowPage("Graphics"), false);
            shot("09_graphics_advanced", null, ui => { ui.ShowPage("Graphics"); ui.OpenAdvanced("Graphics"); ui.FocusRow(S.Backend.Id); }, false);
            // The wheel: the list comes to rest with a whole row at its top, wherever it is left.
            var over = new Point(700, 400);
            shot("09b_graphics_advanced_wheel_1", null, ui => { ui.ShowPage("Graphics"); ui.OpenAdvanced("Graphics"); ui.ScrollTo(0); ui.PointerWheel(over, -120); }, false);
            shot("09c_graphics_advanced_end", null, ui => { ui.ShowPage("Graphics"); ui.OpenAdvanced("Graphics"); ui.ScrollTo(1e6); }, false);
            shot("10_controls_keyboard", null, ui => ui.ShowPage("Controls"), false);
            shot("10b_controls_wheel_1", null, ui => { ui.ShowPage("Controls"); ui.PointerWheel(over, -120); }, false);
            shot("10c_controls_wheel_3", null, ui => { ui.ShowPage("Controls"); for (int i = 0; i < 3; i++) ui.PointerWheel(over, -120); }, false);
            shot("10d_controls_wheel_7", null, ui => { ui.ShowPage("Controls"); for (int i = 0; i < 7; i++) ui.PointerWheel(over, -120); }, false);
            shot("10e_controls_end", null, ui => { ui.ShowPage("Controls"); ui.ScrollTo(1e6); }, false);
            shot("11_controls_actions", null, ui => { ui.ShowPage("Controls"); ui.FocusRow("bind:Cross", 1); }, false);
            shot("12_controls_binding_capture", null, ui => { ui.ShowPage("Controls"); ui.FocusRow("bind:Square", 0); ui.BeginCaptureFor("bind:Square", 0); }, false);
            shot("13_controls_binding_conflict", null, ui =>
            {
                ui.ShowPage("Controls"); ui.FocusRow("bind:Square", 0);
                ui.Ask("Already used", Surface.TakenQuestion("W", "Move forward", "Shoot"), "Move", "Keep", c => { });
            }, false);
            shot("14_controls_no_key", m => { m.SetKeys("Walk", new string[0]); m.SetKeys("DpadUp", new[] { "Up", "W" }); }, ui => { ui.ShowPage("Controls"); ui.FocusRow("bind:Walk", 0); }, false);
            shot("15_controls_aiming", null, ui => { ui.ShowPage("Controls"); ui.FocusRow(S.SensitivityX.Id); }, false);
            shot("15b_controls_mouse", null, ui => { ui.ShowPage("Controls"); ui.ScrollToRow("group:Mouse"); ui.FocusRow(S.Smoothing.Id); }, false);
            shot("16_controls_gamepad", null, ui => { ui.ShowPage("Controls"); ui.SetPadView(true); }, true);
            shot("16b_controls_gamepad_buttons", null, ui => { ui.ShowPage("Controls"); ui.SetPadView(true); ui.FocusRow("bind:Square", 0); ui.SetPadDrove(true); }, true);
            shot("16c_fixed_stick_chip_says_why", null, ui =>
            {
                ui.ShowPage("Controls"); ui.SetPadView(true); ui.ScrollToRow("group:Actions");
                Arrange(ui, w, h);
                var plus = ui.PartRect("bind:MoveForward", 0);
                var chip = new Point(Lay.ControlX + 20, plus.Y + plus.Height / 2);
                ui.PointerDown(chip, System.Windows.Input.MouseButton.Left);
                ui.PointerUp(chip);
            }, true);
            shot("17_controls_gamepad_capture", null, ui => { ui.ShowPage("Controls"); ui.SetPadView(true); ui.FocusRow("bind:Triangle", 0); ui.BeginCaptureFor("bind:Triangle", 0); }, true);
            shot("17b_gamepad_capture_keyboard_used_last", null, ui => { ui.ShowPage("Controls"); ui.SetPadView(true); ui.FocusRow("bind:Triangle", 0); ui.SetDevice(InputDevice.Keyboard); ui.BeginCaptureFor("bind:Triangle", 0); }, true);
            shot("18_controls_gamepad_sticks", null, ui => { ui.ShowPage("Controls"); ui.SetPadView(true); ui.ScrollToRow("group:Sticks and triggers"); }, true);
            shot("19_controls_no_controller", null, ui => { ui.ShowPage("Controls"); ui.SetPadView(true); }, false);
            shot("20_controls_psp_style", m => m.SwitchLayout("psp"), ui => { ui.ShowPage("Controls"); ui.ScrollToRow("bind:Select"); }, false);
            shot("20b_controls_modern_style", m => m.SwitchLayout("modern"), ui => { ui.ShowPage("Controls"); ui.ScrollToRow("group:Actions"); }, false);
            shot("20c_controls_tod_pc_actions", null, ui => { ui.ShowPage("Controls"); ui.ScrollToRow("group:Actions"); }, false);
            shot("20d_controls_tod_pc_buttons", null, ui => { ui.ShowPage("Controls"); ui.ScrollToRow("bind:Square"); }, false);
            shot("20e_controls_tod_pc_dpad", null, ui => { ui.ShowPage("Controls"); ui.ScrollToRow("bind:L"); }, false);
            shot("20f_controls_tod_pc_start", null, ui => { ui.ShowPage("Controls"); ui.ScrollToRow("bind:DpadUp"); }, false);
            shot("21_textures_mods", null, ui => ui.ShowPage("Textures & mods"), false);
            shot("21a_textures_wheel_2", null, ui => { ui.ShowPage("Textures & mods"); ui.PointerWheel(over, -120); ui.PointerWheel(over, -120); }, false);
            shot("21b_textures_one_pack_chosen", m => { foreach (var p in m.TexturePacks) p.Enabled = p.Name == "hd_textures"; m.CommitMods(); }, ui => ui.ShowPage("Textures & mods"), false);
            shot("21c_textures_standard", m => { foreach (var p in m.TexturePacks) p.Enabled = false; m.CommitMods(); }, ui => ui.ShowPage("Textures & mods"), false);
            shot("22_textures_dropdown_open", null, ui => { ui.ShowPage("Textures & mods"); ui.OpenDropdownFor("pack"); }, false);
            shot("23_textures_no_hd_pack", m => { m.Mods.RemoveAll(x => x.HasTextures); }, ui => ui.ShowPage("Textures & mods"), false);
            shot("24_textures_no_mods", m => { m.Mods.Clear(); }, ui => ui.ShowPage("Textures & mods"), false);
            shot("25_sound", null, ui => { ui.ShowPage("Sound"); ui.FocusRow(S.BufferMs.Id); }, false);
            shot("26_game_data", null, ui => ui.ShowPage("Game data"), false);
            shot("27_game_data_bottom", null, ui => { ui.ShowPage("Game data"); ui.OpenAdvanced("Game data"); ui.ScrollTo(1e6); }, false);
            shot("28_defaults_question", null, ui => { ui.ShowPage("Controls"); ui.Ask("Defaults", "Reset keyboard & mouse bindings to the defaults?", "Reset", "Cancel", c => { }, true); }, false);
            shot("29_saved_message", null, ui => { ui.ShowPage("Sound"); ui.SaySaved(); }, false);
            const string locked = "The process cannot access the file because it is being used by another process";
            shot("30_cannot_write", null, ui => { ui.ShowPage("Display"); ui.SayCannotWrite(locked); }, true);
            shot("30b_cannot_write_details", null, ui => { ui.ShowPage("Display"); ui.SayCannotWrite(locked); ui.PressMessageButton(); }, false);
            shot("31_focus_play_button", null, ui => { ui.ShowPage("Play"); ui.FocusBottom(1); ui.SetPadDrove(true); }, true);
            shot("32_focus_rail", null, ui => { ui.ShowPage("Display"); ui.FocusRail(3); }, false);
            shot("32b_focus_rail_selected_tab", null, ui => { ui.ShowPage("Display"); ui.FocusRail(1); }, false);
            // With a controller alone: the focus where the controller put it, and what its buttons do.
            Action<Surface> byPad = ui => ui.SetPadDrove(true);
            shot("33_pad_play", null, ui => { ui.ShowPage("Play"); ui.FocusBottom(1); byPad(ui); }, true);
            shot("34_pad_rail", null, ui => { ui.ShowPage("Play"); ui.FocusRail(2); byPad(ui); }, true);
            shot("35_pad_display", null, ui => { ui.ShowPage("Display"); ui.FocusRow(S.Fullscreen.Id); byPad(ui); }, true);
            shot("36_pad_graphics", null, ui => { ui.ShowPage("Graphics"); ui.FocusRow("quality"); byPad(ui); }, true);
            shot("37_pad_controls_buttons", null, ui => { ui.ShowPage("Controls"); ui.SetPadView(true); ui.FocusRow("bind:Cross", 1); byPad(ui); }, true);
            shot("38_pad_controls_slider", null, ui => { ui.ShowPage("Controls"); ui.SetPadView(true); ui.FocusRow(S.TurnSensitivity.Id); byPad(ui); }, true);
            shot("39_pad_textures", null, ui => { ui.ShowPage("Textures & mods"); ui.FocusRow("mod:pc_camera", 1); byPad(ui); }, true);
            shot("40_pad_dropdown", null, ui => { ui.ShowPage("Textures & mods"); ui.FocusRow("pack"); ui.OpenDropdownFor("pack"); byPad(ui); }, true);
            shot("41_pad_sound", null, ui => { ui.ShowPage("Sound"); ui.FocusRow(S.BufferMs.Id); byPad(ui); }, true);
            shot("42_pad_game_data", null, ui => { ui.ShowPage("Game data"); ui.FocusRow("saves", 1); byPad(ui); }, true);
            shot("43_pad_question", null, ui =>
            {
                ui.ShowPage("Controls"); ui.SetPadView(true); ui.FocusRow("bind:Triangle", 0); byPad(ui);
                ui.Ask("Already used", Surface.TakenQuestion("Y", "Jump / dive / use", "Reload"), "Move", "Keep", c => { });
            }, true);
            // The same pages with a PlayStation controller (a DualSense): its pictures in the chips and along the bottom.
            Action<string, Action<Model>, Action<Surface>> ps = (name, prepare, act) =>
            {
                padKind = PadKind.PlayStation; padPid = 0x0CE6;
                shot(name, prepare, act, true);
                padKind = PadKind.Xbox; padPid = 0;
            };
            ps("44_ps_controls_buttons", null, ui => { ui.ShowPage("Controls"); ui.SetPadView(true); ui.FocusRow("bind:Cross", 1); byPad(ui); });
            ps("45_ps_controls_capture", null, ui => { ui.ShowPage("Controls"); ui.SetPadView(true); ui.FocusRow("bind:Triangle", 0); byPad(ui); ui.BeginCaptureFor("bind:Triangle", 0); });
            ps("46_ps_question", null, ui =>
            {
                ui.ShowPage("Controls"); ui.SetPadView(true); ui.FocusRow("bind:Triangle", 0); byPad(ui);
                ui.Ask("Already used", Surface.TakenQuestion(KeyNames.PadDisplay("Y", ui.Look, ui.PadSaysCreate), "Jump / dive / use", "Reload"), "Move", "Keep", c => { });
            });
            ps("47_ps_play", null, ui => { ui.ShowPage("Play"); ui.FocusBottom(1); byPad(ui); });
            ps("48_ps_slider", null, ui => { ui.ShowPage("Controls"); ui.SetPadView(true); ui.FocusRow(S.TurnSensitivity.Id); byPad(ui); });
            ps("49a_ps_buttons_shoot_to_next_weapon", m => m.Set(S.Pad("RS"), "Circle"), ui => { ui.ShowPage("Controls"); ui.SetPadView(true); byPad(ui); ui.ScrollToRow("bind:Square"); });
            ps("49b_ps_buttons_rewind_to_select", m => m.Set(S.Pad("RS"), "Circle"), ui => { ui.ShowPage("Controls"); ui.SetPadView(true); byPad(ui); ui.ScrollToRow("bind:DpadDown"); });
            // The new rows of the Controls page, and what "Gamepad buttons" does to the launcher's own pictures.
            shot("50_controls_controller_rows", null, ui => { ui.ShowPage("Controls"); ui.SetPadView(true); ui.FocusRow(S.Rumble.Id); byPad(ui); }, true);
            shot("51_controls_prompts_rows", null, ui => { ui.ShowPage("Controls"); ui.FocusRow(S.Prompts.Id); ui.SetDevice(InputDevice.Keyboard); }, false);
            shot("52_gamepad_buttons_playstation_with_xbox_pad", m => m.Set(S.PadGlyphs, "playstation"), ui => { ui.ShowPage("Controls"); ui.SetPadView(true); ui.FocusRow("bind:Square", 0); byPad(ui); }, true);
            ps("53_gamepad_buttons_xbox_with_playstation_pad", m => m.Set(S.PadGlyphs, "xbox"), ui => { ui.ShowPage("Controls"); ui.SetPadView(true); ui.FocusRow("bind:Square", 0); byPad(ui); });
            shot("54_rumble_test_no_controller", null, ui => { ui.ShowPage("Controls"); ui.SetPadView(true); ui.FocusRow(S.Rumble.Id); ui.SetDevice(InputDevice.Keyboard); ui.DoNav(Nav.Activate, false); }, false);
            // The keyboard used last: key caps along the bottom. The mouse used last: nothing there.
            shot("55_keyboard_prompts_row", null, ui => { ui.ShowPage("Display"); ui.FocusRow(S.FrameLock.Id); ui.SetDevice(InputDevice.Keyboard); }, true);
            shot("56_keyboard_prompts_slider", null, ui => { ui.ShowPage("Sound"); ui.FocusRow(S.BufferMs.Id); ui.SetDevice(InputDevice.Keyboard); }, false);
            shot("57_keyboard_prompts_chip", null, ui => { ui.ShowPage("Controls"); ui.FocusRow("bind:Cross", 1); ui.SetDevice(InputDevice.Keyboard); }, false);
            shot("58_mouse_used_last_no_prompts", null, ui => { ui.ShowPage("Display"); ui.SetDevice(InputDevice.Mouse); }, true);
            // Locked: the padlock, and the reason in the message line once the player tries (A or Enter, or a click).
            shot("59_locked_row_focus_says_nothing", m => m.Set(S.InternalScale, "2"), ui => { ui.ShowPage("Display"); ui.OpenAdvanced("Display"); ui.FocusRow(S.Preset.Id); ui.SetDevice(InputDevice.Keyboard); }, false);
            shot("60_locked_row_enter_says_why", m => m.Set(S.InternalScale, "2"), ui => { ui.ShowPage("Display"); ui.OpenAdvanced("Display"); ui.FocusRow(S.Preset.Id); ui.SetDevice(InputDevice.Keyboard); ui.DoNav(Nav.Activate, false); }, false);
            shot("61_locked_report_over_error", m => m.Set(S.InternalScale, "2"), ui =>
            {
                ui.ShowPage("Display"); ui.OpenAdvanced("Display"); ui.SayCannotWrite(locked);
                ui.FocusRow(S.Preset.Id); ui.SetDevice(InputDevice.Keyboard); ui.DoNav(Nav.Activate, false);
            }, false);
            shot("62_dimmed_choice_says_why", m => { m.Mods.RemoveAll(x => x.HasTextures); }, ui =>
            {
                ui.ShowPage("Textures & mods");
                Arrange(ui, w, h);
                var hd = ui.PartRect("textures", 1);
                ui.PointerDown(new Point(hd.X + hd.Width / 2, hd.Y + hd.Height / 2), System.Windows.Input.MouseButton.Left);
                ui.PointerUp(new Point(hd.X + hd.Width / 2, hd.Y + hd.Height / 2));
            }, false);
            shot("62b_dimmed_choice_enter_says_why", m => { m.Mods.RemoveAll(x => x.HasTextures); }, ui => { ui.ShowPage("Textures & mods"); ui.FocusRow("textures"); ui.SetDevice(InputDevice.Keyboard); ui.DoNav(Nav.Activate, false); }, false);
            shot("63_locked_pack_row", m => { foreach (var p in m.TexturePacks) p.Enabled = false; m.CommitMods(); }, ui => { ui.ShowPage("Textures & mods"); ui.FocusRow("pack"); ui.SetDevice(InputDevice.Keyboard); ui.DoNav(Nav.Activate, false); }, false);
            shot("64_empty_mod_locked", null, ui => { ui.ShowPage("Textures & mods"); ui.FocusRow("mod:work_in_progress", 0); ui.SetDevice(InputDevice.Keyboard); ui.DoNav(Nav.Activate, false); }, false);
            shot("65_play_game_not_found", m => { m.Game = new GameInfo { ExePath = Path.Combine(m.BaseDir, "ChiliConCarnage.exe") }; }, ui => ui.ShowPage("Play"), false);
            shot("65b_skip_this_window_on", m => m.Config.Set("StartGameDirectly", "true"), ui => { ui.ShowPage("Play"); ui.FocusRow("direct"); ui.SetDevice(InputDevice.Keyboard); }, false);
            shot("66_play_never_played", null, ui => { ui.ShowPage("Play"); ui.HideLastPlayed = true; }, false);
            if (texts.Length > 0)
            {
                var titled = new Model(baseDir, ini);
                titled.InspectGame();
                string head = "the window's title bar (not in the pictures)\r\n    " + new Surface(titled, null) { Static = true }.WindowTitle() + "\r\n";
                File.WriteAllText(Path.Combine(outDir, string.IsNullOrEmpty(only) ? "texts.txt" : "texts_" + only + ".txt"), head + texts.ToString());
            }
            Console.WriteLine(count + " pictures in " + outDir);
            return 0;
        }
    }

    /// <summary>Tests of the settings reader and writer.</summary>
    public static class IniTests
    {
        static int failed, passed;
        static void Check(string name, bool ok, string detail = "")
        {
            if (ok) passed++; else failed++;
            Console.WriteLine((ok ? "PASS  " : "FAIL  ") + name + (ok || detail.Length == 0 ? "" : "   " + detail));
        }
        static int DifferentLines(byte[] a, byte[] b, out int added)
        {
            var la = System.Text.Encoding.GetEncoding(28591).GetString(a).Split('\n');
            var lb = System.Text.Encoding.GetEncoding(28591).GetString(b).Split('\n');
            added = lb.Length - la.Length;
            if (added != 0)
            {
                // Lines of b not in a (by position after a simple longest-common-prefix/suffix cut).
                int pre = 0;
                while (pre < la.Length && pre < lb.Length && la[pre] == lb[pre]) pre++;
                int suf = 0;
                while (suf < la.Length - pre && suf < lb.Length - pre && la[la.Length - 1 - suf] == lb[lb.Length - 1 - suf]) suf++;
                return Math.Max(la.Length, lb.Length) - pre - suf;
            }
            int n = 0;
            for (int i = 0; i < la.Length; i++) if (la[i] != lb[i]) n++;
            return n;
        }
        static string Text(byte[] b) { return System.Text.Encoding.UTF8.GetString(b); }

        public static int Run(string realIni)
        {
            string temp = Path.Combine(Path.GetTempPath(), "chili_launcher_ini_test_" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(temp);
            try
            {
                byte[] real = realIni != null && File.Exists(realIni) ? File.ReadAllBytes(realIni) : null;
                if (real != null)
                {
                    Console.WriteLine("real file: " + realIni + " (" + real.Length + " bytes)");
                    Check("the real ini round-trips byte for byte", IniDocument.Parse(real).ToBytes().SequenceEqual(real));
                    string path = Path.Combine(temp, "ChiliNative.ini");
                    File.WriteAllBytes(path, real);
                    var stamp = File.GetLastWriteTimeUtc(path);
                    var m = new Model(temp, null);
                    Check("the launcher reads Scheme from it", m.Get(S.Scheme) == IniDocument.Parse(real).Get("Input", "Scheme"));
                    string err = m.Save();
                    Check("saving with nothing changed leaves the file alone", err == null && File.ReadAllBytes(path).SequenceEqual(real) && File.GetLastWriteTimeUtc(path) == stamp);

                    // Setting every control to the value it shows must not change a byte either.
                    foreach (var s in S.All) { if (s == S.ModsEnabled) continue; m.Set(s, m.Get(s)); }
                    Check("setting every key to its current value changes nothing", !m.AnyChanged && m.Save() == null && File.ReadAllBytes(path).SequenceEqual(real));

                    // One live key.
                    string old = m.Get(S.SensitivityX);
                    m.Set(S.SensitivityX, "0.12");
                    m.Save();
                    byte[] after = File.ReadAllBytes(path);
                    int added;
                    int diff = DifferentLines(real, after, out added);
                    Check("changing a live key (SensitivityX) changes exactly one line", diff == 1 && added == 0 && Text(after).Contains("SensitivityX=0.12"), "lines changed: " + diff + ", added: " + added);
                    m.Set(S.SensitivityX, old);
                    m.Save();
                    Check("changing it back restores the file byte for byte", File.ReadAllBytes(path).SequenceEqual(real));

                    // A key that is in the file switched off with ';'.
                    var doc = IniDocument.Parse(real);
                    if (!doc.Has("Rendering", "Fullscreen") && doc.Mentions("Rendering", "Fullscreen"))
                    {
                        m.Set(S.Fullscreen, "true");
                        m.Save();
                        after = File.ReadAllBytes(path);
                        diff = DifferentLines(real, after, out added);
                        Check("a switched-off key (; Fullscreen=false) becomes one live line", diff == 1 && added == 0 && Text(after).Contains("\nFullscreen=true"), "lines changed: " + diff + ", added: " + added);
                        Check("the game would now read Fullscreen=true", IniDocument.Parse(after).Get("Rendering", "Fullscreen") == "true");
                        File.WriteAllBytes(path, real);
                        m.Reload();
                    }
                    // A key the file does not have at all.
                    if (!doc.Mentions("Rendering", "Anisotropy"))
                    {
                        m.Set(S.Anisotropy, "8");
                        m.Save();
                        after = File.ReadAllBytes(path);
                        diff = DifferentLines(real, after, out added);
                        Check("a key the file lacks (Anisotropy) adds exactly one line, inside [Rendering]", added == 1 && diff == 1 && IniDocument.Parse(after).Get("Rendering", "Anisotropy") == "8", "lines changed: " + diff + ", added: " + added);
                        File.WriteAllBytes(path, real);
                        m.Reload();
                    }
                    // A binding.
                    m.SetKeys("Square", new[] { "G", "H" });
                    m.Save();
                    after = File.ReadAllBytes(path);
                    diff = DifferentLines(real, after, out added);
                    Check("a binding (Square=G,H) changes exactly one line", diff == 1 && added == 0 && Text(after).Contains("Square=G,H"));
                    File.WriteAllBytes(path, real);
                    m.Reload();
                    // The mods list.
                    Directory.CreateDirectory(Path.Combine(temp, "mods", "alpha"));
                    Directory.CreateDirectory(Path.Combine(temp, "mods", "beta", "textures"));
                    m.Reload();
                    Check("two mod folders are found, both on, one a texture pack", m.Mods.Count == 2 && m.Mods.All(x => x.Enabled) && m.TexturePacks.Count == 1);
                    m.Mods[1].Enabled = false;
                    m.CommitMods();
                    m.Save();
                    after = File.ReadAllBytes(path);
                    diff = DifferentLines(real, after, out added);
                    Check("switching a mod off writes one line, Enabled=alpha", diff == 1 && IniDocument.Parse(after).Get("Mods", "Enabled") == "alpha", "lines changed: " + diff);
                    m.Mods[1].Enabled = true;
                    m.CommitMods();
                    m.Save();
                    after = File.ReadAllBytes(path);
                    Check("all mods on again: the Enabled line is switched off, so the game loads every folder", !IniDocument.Parse(after).Has("Mods", "Enabled"));

                    // The control styles. The sample is a file the game wrote with [Input] Version 2: no Layout line.
                    File.WriteAllBytes(path, real);
                    m = new Model(temp, null);
                    Check("a Version 2 file without a Layout line: Total Overdose PC by its scheme, the keys as the file has them",
                        m.Layout == "tod_pc" && m.Get(S.Keys("Cross")) == "Space,E,Enter" && !m.AnyChanged);
                    // Brought up to date as the game does (host/pc_input.hpp, brought_up_to_date), to the bytes the game makes:
                    // sample_ChiliNative.migrated.ini beside the sample, which tools/pc_input_test.cpp holds the game to as well.
                    string bak = path + ".bak";
                    File.WriteAllText(bak, "an older backup");
                    string done = m.MigrateInput();
                    byte[] migrated = File.ReadAllBytes(path);
                    string expectedFile = Path.Combine(Path.GetDirectoryName(realIni), Path.GetFileNameWithoutExtension(realIni) + ".migrated.ini");
                    Check("every binding the older build's default: Total Overdose PC's now, Layout=tod_pc, Version=3",
                        done == "tod_pc" && m.Layout == "tod_pc" && m.Doc.Get("Input", "Layout") == "tod_pc" && m.Doc.Get("Input", "Version") == "3" &&
                        m.Get(S.Keys("Cross")) == "Space,F,Enter,LShift" && m.Get(S.Keys("Circle")) == "E,Backspace" && m.Get(S.Keys("Select")) == "Grave" && !m.AnyChanged);
                    Check("the same bytes as the game makes of the file", File.Exists(expectedFile) && File.ReadAllBytes(expectedFile).SequenceEqual(migrated), expectedFile);
                    Check("the file as it was is the .bak, over an older one", File.ReadAllBytes(bak).SequenceEqual(real));
                    Check("its comments about the older build and about Cross say the new thing",
                        !Text(migrated).Contains("E uses") && Text(migrated).Contains("; Jump, dive, use, enter a vehicle, confirm: the game has them all on Cross.") &&
                        !Text(migrated).Contains("Delete this file after changing the scheme"));
                    Check("a file brought up to date is not brought up to date again", m.MigrateInput() == null && File.ReadAllBytes(path).SequenceEqual(migrated));
                    // Switching the style: the bindings that still hold the old style's default, and only those.
                    m.SetKeys("Square", new[] { "G", "H" });
                    m.SwitchLayout("modern");
                    Check("Total Overdose PC to Modern: Modern's keys, the player's own Square kept, the scheme stays",
                        m.Layout == "modern" && m.Get(S.Keys("Cross")) == "Space,E,Enter" && m.Get(S.Keys("Circle")) == "F,Backspace" && m.Get(S.Keys("DpadLeft")) == "Left,1" &&
                        m.Get(S.Keys("Square")) == "G,H" && m.Scheme == "dual_analog" && m.Get(S.Pad("X")) == "Triangle");
                    m.SwitchLayout("psp");
                    Check("to PSP original: the PSP's scheme and pad table, no look keys",
                        m.Layout == "psp" && m.Scheme == "psp" && m.Get(S.Keys("LookLeft")) == "" && m.Get(S.Pad("X")) == "Square" && m.Get(S.Pad("LS")) == "None" &&
                        m.Get(S.Keys("Cross")) == "Space,E,Enter" && m.Get(S.Keys("Square")) == "G,H" && m.Get(S.Camera) == "original");
                    m.SwitchLayout("tod_pc");
                    Check("and back to Total Overdose PC", m.Layout == "tod_pc" && m.Scheme == "dual_analog" && m.Get(S.Keys("Cross")) == "Space,F,Enter,LShift" &&
                        m.Get(S.Keys("LookLeft")) == "J" && m.Get(S.Pad("X")) == "Triangle" && m.Get(S.Keys("Square")) == "G,H");
                    m.Save();
                    var saved = IniDocument.Parse(File.ReadAllBytes(path));
                    Check("saved: the file holds the style and the player's own key", saved.Get("Input", "Layout") == "tod_pc" && saved.Get("Input", "Scheme") == "dual_analog" &&
                        saved.Get("Input.Keyboard", "Square") == "G,H" && saved.Get("Input.Keyboard", "Cross") == "Space,F,Enter,LShift");
                    m.ResetKeyboardMouse();
                    Check("Defaults gives the style's keys", m.Get(S.Keys("Square")) == "G" && m.Get(S.Keys("Triangle")) == "R,LCtrl");
                    // A binding of the player's own in an older file: every binding kept, the style it was made with.
                    File.WriteAllBytes(path, System.Text.Encoding.UTF8.GetBytes(Text(real).Replace("Square=G", "Square=G,H")));
                    m = new Model(temp, null);
                    done = m.MigrateInput();
                    Check("a binding of the player's own: every binding kept, Modern", done == "modern" && m.Layout == "modern" && m.Get(S.Keys("Cross")) == "Space,E,Enter" &&
                        m.Get(S.Keys("Square")) == "G,H" && m.Doc.Get("Input", "Version") == "3" && Text(File.ReadAllBytes(path)).Contains("; Space jumps and dives, E uses: the game has both on Cross."));
                    // A hand-written file (not the game's first line) is never touched.
                    string handWritten = "[Input]\r\nVersion=2\r\nScheme=dual_analog\r\n";
                    File.WriteAllText(path, handWritten);
                    m = new Model(temp, null);
                    Check("a hand-written file is not touched", m.MigrateInput() == null && File.ReadAllText(path) == handWritten && m.Layout == "tod_pc");
                    File.WriteAllBytes(path, real);
                    try { File.Delete(bak); } catch { }
                }
                else Console.WriteLine("(no real ini given: only the made-up files are tested)");

                // Made-up files: odd but legal things.
                string odd = "\uFEFF; top comment\n\n[Unknown.Section]\nFoo = bar ; trailing\n  Weird Key=1\njust text\n[Rendering]\n  Backend = software   ; the slow one\n#MSAA=4\n[Game]\nFrameLock=2";
                byte[] oddBytes = new System.Text.UTF8Encoding(true).GetPreamble().Concat(new System.Text.UTF8Encoding(false).GetBytes(odd.Substring(1))).ToArray();
                var d = IniDocument.Parse(oddBytes);
                Check("BOM, LF line ends, no final newline, unknown section and stray text round-trip", d.ToBytes().SequenceEqual(oddBytes));
                Check("inline comment is not part of the value", d.Get("Rendering", "Backend") == "software" && d.Get("Unknown.Section", "Foo") == "bar");
                d.Set("Rendering", "Backend", "d3d12");
                Check("a changed value keeps its indentation, spacing and comment", Text(d.ToBytes()).Contains("\n  Backend = d3d12   ; the slow one\n"));
                d.Set("Rendering", "MSAA", "8");
                Check("'#MSAA=4' becomes 'MSAA=8'", Text(d.ToBytes()).Contains("\nMSAA=8\n") && !Text(d.ToBytes()).Contains("#MSAA"));
                d.Set("Game", "Camera", "tight");
                Check("a new key after a last line without a newline", Text(d.ToBytes()).EndsWith("FrameLock=2\nCamera=tight\n"));
                d.Set("Audio", "BufferMs", "100");
                Check("a new section goes to the end", Text(d.ToBytes()).EndsWith("\n\n[Audio]\nBufferMs=100\n"));
                d.Unset("Game", "FrameLock");
                Check("switching a key off puts '; ' in front", Text(d.ToBytes()).Contains("\n; FrameLock=2\n") && d.Get("Game", "FrameLock") == null);
                Check("unknown lines are still there, untouched", Text(d.ToBytes()).Contains("[Unknown.Section]\nFoo = bar ; trailing\n  Weird Key=1\njust text\n"));

                var crlf = IniDocument.Parse(System.Text.Encoding.ASCII.GetBytes("[Disc]\r\n; Path=C:\\Games\\x.iso\r\n"));
                crlf.Set("Disc", "Path", "D:\\My #1 Games; old\\Chili.iso", true);
                Check("a path keeps ';' and '#', and CRLF line ends stay", Text(crlf.ToBytes()) == "[Disc]\r\nPath=D:\\My #1 Games; old\\Chili.iso\r\n" && crlf.GetRaw("Disc", "Path") == "D:\\My #1 Games; old\\Chili.iso");

                var latin = IniDocument.Parse(new byte[] { (byte)'[', (byte)'A', (byte)']', 10, (byte)'K', (byte)'=', 0xE9, 0xFF, 10 });
                Check("bytes that are not UTF-8 survive", latin.ToBytes().SequenceEqual(new byte[] { (byte)'[', (byte)'A', (byte)']', 10, (byte)'K', (byte)'=', 0xE9, 0xFF, 10 }));

                var quoted = IniDocument.Parse(System.Text.Encoding.ASCII.GetBytes("[Disc]\nPath=\"C:/Games/Chili.iso\"\n[Launcher]\nArguments=\"{boot}\" \"{disc}\" --ms0 \"{saves}\"\n"));
                Check("a quoted path loses its quotes, a command line of quoted words keeps them",
                    quoted.GetRaw("Disc", "Path") == "C:/Games/Chili.iso" && quoted.GetRaw("Launcher", "Arguments") == "\"{boot}\" \"{disc}\" --ms0 \"{saves}\"");

                var empty = new IniDocument();
                empty.Set("Rendering", "Fullscreen", "true");
                Check("an empty file gets a section and a key", Text(empty.ToBytes()) == "[Rendering]\r\nFullscreen=true\r\n");

                var twice = IniDocument.Parse(System.Text.Encoding.ASCII.GetBytes("[Game]\nFrameLock=1\nFrameLock=2\n"));
                Check("the last of two lines is the one in force", twice.Get("Game", "FrameLock") == "2");

                // A settings file that is gone for a moment is not a file that was never there: the launcher
                // never puts its few changed lines in the place of the whole file.
                {
                    string dir = Path.Combine(temp, "gone");
                    Directory.CreateDirectory(dir);
                    string path = Path.Combine(dir, "ChiliNative.ini");
                    string whole = "; the player's own file\r\n[Disc]\r\nPath=C:\\Games\\Chili.iso\r\n\r\n[Input.Mouse]\r\nSensitivityX=0.08\r\nInvertY=true\r\n\r\n[Rendering]\r\nAnisotropy=16\r\n";
                    File.WriteAllText(path, whole);
                    var m = new Model(dir, null);
                    // Gone for good while the launcher is open (another writer's swap failed half way).
                    File.Delete(path);
                    m.Set(S.SensitivityX, "0.12");
                    string err = m.Save();
                    Check("the file gone at a save: written again whole, with the change",
                        err == null && File.Exists(path) && File.ReadAllText(path) == whole.Replace("SensitivityX=0.08", "SensitivityX=0.12"), err ?? (File.Exists(path) ? File.ReadAllText(path) : "no file"));
                    // Gone for a moment at the read (it comes back while the launcher waits): the file as it came back, with the change.
                    string moved = path + ".away";
                    Model.SaveBeforeRead = () =>
                    {
                        Model.SaveBeforeRead = null;
                        File.Move(path, moved);
                        var back = new System.Threading.Thread(() => { System.Threading.Thread.Sleep(120); File.WriteAllText(moved, File.ReadAllText(moved).Replace("Anisotropy=16", "Anisotropy=4")); File.Move(moved, path); });
                        back.IsBackground = true;
                        back.Start();
                    };
                    m.Set(S.SensitivityX, "0.2");
                    err = m.Save();
                    var got = File.Exists(path) ? File.ReadAllText(path) : "no file";
                    Check("the file gone for a moment at a save: waited for, the other writer's line kept",
                        err == null && got == whole.Replace("SensitivityX=0.08", "SensitivityX=0.2").Replace("Anisotropy=16", "Anisotropy=4"), err ?? got);
                    // Gone at a reload (the game ended and its last swap failed half way), then a save.
                    File.Delete(path);
                    m.Reload();
                    Check("the file gone at a reload: the launcher still shows what it read", m.IniExists && m.Get(S.SensitivityX) == "0.2" && m.Get(S.Anisotropy) == "4");
                    m.Set(S.Anisotropy, "8");
                    err = m.Save();
                    got = File.Exists(path) ? File.ReadAllText(path) : "no file";
                    Check("and the next save writes the whole file", err == null && got == whole.Replace("SensitivityX=0.08", "SensitivityX=0.2").Replace("Anisotropy=16", "Anisotropy=8"), err ?? got);
                    // A program that has the file open for reading only (no sharing for a swap or a write) lets go
                    // after a moment: the save waits and the file is whole.
                    var holder = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
                    var letGo = new System.Threading.Thread(() => { System.Threading.Thread.Sleep(60); holder.Dispose(); });
                    letGo.IsBackground = true;
                    letGo.Start();
                    m.Set(S.Anisotropy, "2");
                    err = m.Save();
                    letGo.Join();
                    got = File.ReadAllText(path);
                    Check("a reader that holds the file for a moment: the save waits, the file is whole",
                        err == null && got == whole.Replace("SensitivityX=0.08", "SensitivityX=0.2").Replace("Anisotropy=16", "Anisotropy=2"), err ?? got);
                    Check("no temporary file is left", !File.Exists(path + ".launcher.tmp"));
                    // A reader that never lets go: the save says why, the file is as it was, and the change is still pending.
                    using (var stuck = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read))
                    {
                        m.Set(S.Anisotropy, "16");
                        err = m.Save();
                    }
                    Check("a reader that never lets go: an error, the file as it was, the change kept for the next save",
                        err != null && m.AnyChanged && File.ReadAllText(path) == got && !File.Exists(path + ".launcher.tmp"), err ?? "saved");
                    err = m.Save();
                    Check("the next save writes it", err == null && File.ReadAllText(path) == got.Replace("Anisotropy=2", "Anisotropy=16"), err ?? File.ReadAllText(path));
                    // A launcher that never saw a file still makes one from nothing (the game's first start does the same).
                    string fresh = Path.Combine(temp, "fresh");
                    Directory.CreateDirectory(fresh);
                    var first = new Model(fresh, null);
                    first.Set(S.Anisotropy, "8");
                    err = first.Save();
                    Check("no file yet: the first save makes one", err == null && File.ReadAllText(Path.Combine(fresh, "ChiliNative.ini")) == "[Rendering]\r\nAnisotropy=8\r\n", err ?? "");
                }

                // The game program's capabilities are read from its text.
                var data = System.Text.Encoding.ASCII.GetBytes("xx\0Camera must be tight or original\0PresetX\0 AntiAliasing must be off\0ExactTurn\0");
                var found = GameInfo.Scan(data, new List<string> { "Camera must be", "Preset", "AntiAliasing must be", "Exact", "SSAO must be" });
                Check("capability scan finds whole words only", found.Contains("Camera must be") && found.Contains("AntiAliasing must be") && !found.Contains("Preset") && !found.Contains("Exact") && !found.Contains("SSAO must be"));

                // The disc check.
                string disc = Path.Combine(temp, "disc0");
                Directory.CreateDirectory(Path.Combine(disc, "PSP_GAME", "SYSDIR"));
                File.WriteAllText(Path.Combine(disc, "UMD_DATA.BIN"), "ULUS-10216|0123456789ABCDEF|0001|G|");
                File.WriteAllText(Path.Combine(disc, "PSP_GAME", "SYSDIR", "BOOT.BIN"), "x");
                Check("a disc folder with the right id is accepted", DiscCheck.Check(disc).Ok && DiscCheck.Check(Path.Combine(disc, "PSP_GAME", "SYSDIR")).Ok);
                File.WriteAllText(Path.Combine(disc, "UMD_DATA.BIN"), "ULES-00629|0123456789ABCDEF|0001|G|");
                Check("another disc id is refused with the reason", DiscCheck.Check(disc).State == DiscState.WrongDisc && DiscCheck.Check(disc).Message == DiscCheck.WrongText);
                File.WriteAllText(Path.Combine(temp, "game.cso"), "CISO");
                Check("a .cso is refused with the reason", DiscCheck.Check(Path.Combine(temp, "game.cso")).State == DiscState.Unreadable);
                byte[] image = MakeIso("ULUS-10216|0123456789ABCDEF|0001|G|");
                File.WriteAllBytes(Path.Combine(temp, "game.iso"), image);
                var checkedIso = DiscCheck.Check(Path.Combine(temp, "game.iso"));
                Check("a made-up .iso with the right id is accepted", checkedIso.Ok, checkedIso.State + " " + checkedIso.Message);
                File.WriteAllBytes(Path.Combine(temp, "other.iso"), MakeIso("NPUH-12345|0123456789ABCDEF|0001|G|"));
                Check("a made-up .iso of another game is refused", DiscCheck.Check(Path.Combine(temp, "other.iso")).State == DiscState.WrongDisc);
            }
            catch (Exception e) { Check("no exception", false, e.ToString()); }
            finally { try { Directory.Delete(temp, true); } catch { } }
            // Controllers and prompts: what can be checked without a window and without a controller.
            try { PadTests.Run((name, ok, detail) => Check(name, ok, detail ?? "")); }
            catch (Exception e) { Check("no exception in the controller tests", false, e.ToString()); }
            Console.WriteLine(passed + " passed, " + failed + " failed");
            return failed == 0 ? 0 : 1;
        }

        /// <summary>A minimal ISO 9660 image: UMD_DATA.BIN and PSP_GAME/SYSDIR/BOOT.BIN.</summary>
        static byte[] MakeIso(string umd)
        {
            const int S = 2048;
            var img = new byte[S * 24];
            Action<int, int, uint> le = (offset, at, value) => { BitConverter.GetBytes(value).CopyTo(img, offset + at); };
            Func<int, string, uint, uint, bool, int> record = (offset, name, lba, size, dir) =>
            {
                int len = 33 + name.Length + ((name.Length % 2 == 0) ? 1 : 0);
                img[offset] = (byte)len;
                le(offset, 2, lba); le(offset, 10, size);
                img[offset + 25] = (byte)(dir ? 2 : 0);
                img[offset + 32] = (byte)name.Length;
                System.Text.Encoding.ASCII.GetBytes(name).CopyTo(img, offset + 33);
                return offset + len;
            };
            // Primary volume descriptor at sector 16; root directory at 18, PSP_GAME at 19, SYSDIR at 20, files at 21 and 22.
            img[16 * S] = 1;
            System.Text.Encoding.ASCII.GetBytes("CD001").CopyTo(img, 16 * S + 1);
            record(16 * S + 156, "\0", 18, S, true);
            int p = 18 * S;
            p = record(p, "\0", 18, S, true);
            p = record(p, "\u0001", 18, S, true);
            p = record(p, "PSP_GAME", 19, S, true);
            p = record(p, "UMD_DATA.BIN;1", 21, (uint)umd.Length, false);
            p = 19 * S;
            p = record(p, "\0", 19, S, true);
            p = record(p, "\u0001", 18, S, true);
            p = record(p, "SYSDIR", 20, S, true);
            p = 20 * S;
            p = record(p, "\0", 20, S, true);
            p = record(p, "\u0001", 19, S, true);
            p = record(p, "BOOT.BIN;1", 22, 4, false);
            System.Text.Encoding.ASCII.GetBytes(umd).CopyTo(img, 21 * S);
            return img;
        }
    }
}
