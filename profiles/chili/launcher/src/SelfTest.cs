// --selftest <folder>: the real window, driven through the same entry points as the keyboard, the
// mouse and the Play button, without taking the focus or the pointer. It checks what a person would
// otherwise click through, writes selftest.txt and a few pictures of the live window, and exits with
// the number of failed checks. Run it on a made-up game folder (shots.ps1 makes one in bin\fixture).
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Windows;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;

namespace ChiliLauncher
{
    public static class SelfTest
    {
        static readonly List<string> log = new List<string>();
        static int failed;
        static string outDir;
        static MainWindow window;
        static Surface ui;
        static Model model;

        static void Check(string name, bool ok, string detail = "")
        {
            if (!ok) failed++;
            log.Add((ok ? "PASS  " : "FAIL  ") + name + (detail.Length > 0 ? "   [" + detail + "]" : ""));
        }
        static void Note(string text) { log.Add("      " + text); }

        static void Shot(string name)
        {
            try
            {
                ui.InvalidateVisual();
                ui.UpdateLayout();
                var source = PresentationSource.FromVisual(ui);
                double k = source != null ? source.CompositionTarget.TransformToDevice.M11 : 1;
                var bitmap = new RenderTargetBitmap((int)Math.Round(ui.ActualWidth * k), (int)Math.Round(ui.ActualHeight * k), 96 * k, 96 * k, PixelFormats.Pbgra32);
                bitmap.Render(ui);
                var encoder = new PngBitmapEncoder();
                encoder.Frames.Add(BitmapFrame.Create(bitmap));
                using (var f = File.Create(Path.Combine(outDir, name + ".png"))) encoder.Save(f);
            }
            catch (Exception e) { Note("picture " + name + " failed: " + e.Message); }
        }

        static void Key(Key key, ModifierKeys mods = ModifierKeys.None) { ui.HandleKey(key, mods, false); }
        static void Click(Rect r, MouseButton button = MouseButton.Left)
        {
            var p = new Point(r.X + r.Width / 2, r.Y + r.Height / 2);
            ui.PointerMove(p);
            ui.PointerDown(p, button);
            if (button == MouseButton.Left) ui.PointerUp(p);
        }
        static string Ini(string section, string key) { return IniDocument.Load(model.IniPath).Get(section, key); }
        static void Pad(ushort buttons, byte lt = 0)
        {
            ui.PadUpdate(new PadState { Connected = true, Index = 0, Buttons = buttons, LT = lt }, true);
        }
        /// <summary>A PlayStation controller's report, made by hand, through the decoder and the position map into the window.</summary>
        static void PsReport(string hex, int length = 64)
        {
            ui.PadUpdate(PadTests.State(0x0CE6, hex, length), true);
        }
        const string PsRest = "01 80 80 80 80 00 00 00 08 00 00";
        const string PsCross = "01 80 80 80 80 00 00 00 28 00 00";
        const string PsCircle = "01 80 80 80 80 00 00 00 48 00 00";
        const string PsL3 = "01 80 80 80 80 00 00 00 08 40 00";
        /// <summary>Every text the window draws now.</summary>
        static List<string> Drawn()
        {
            Tx.Log = new List<string>();
            ui.InvalidateVisual();
            ui.UpdateLayout();
            var texts = Tx.Log;
            Tx.Log = null;
            return texts;
        }
        static void ClickAt(double x, double y)
        {
            var p = new Point(x, y);
            ui.PointerMove(p); ui.PointerDown(p, MouseButton.Left); ui.PointerUp(p);
        }

        public static int Run(Application app, MainWindow w, Model m, string folder)
        {
            window = w; ui = w.Surface; model = m; outDir = folder;
            Directory.CreateDirectory(outDir);
            w.ShowActivated = false;
            w.Quiet = true;
            ui.IgnoreMouse = true;
            var steps = new Queue<KeyValuePair<int, Action>>();
            Action<int, Action> then = (ms, a) => steps.Enqueue(new KeyValuePair<int, Action>(ms, a));
            string fake = Path.Combine(model.GameDir, "fakegame.txt");
            var over = new Point(700, 400);

            then(900, () =>
            {
                var source = PresentationSource.FromVisual(ui);
                double k = source != null ? source.CompositionTarget.TransformToDevice.M11 : 0;
                Note("the window is " + (ui.WindowActive ? "IN FRONT (it took the focus)" : "not in front (it did not take the focus)"));
                Note("window client " + ui.ActualWidth.ToString("0") + " x " + ui.ActualHeight.ToString("0") + " units, " + (ui.ActualWidth * k).ToString("0") + " x " + (ui.ActualHeight * k).ToString("0") + " pixels, scale " + k.ToString("0.00"));
                Check("the window opens with a 16:9 picture of at least 960 x 540", ui.ActualWidth >= 959 && Math.Abs(ui.ActualWidth / ui.ActualHeight - 16.0 / 9) < 0.02);
                Check("the game program was found", model.Game.Exists, model.Game.ExePath);
                Check("game data was found", model.Disc.Ok, model.Disc.Path);
                Check("the window's title bar: the game's name first, then the launcher's and the game's version",
                    window.Title.StartsWith("Chili Con Carnage") && window.Title.Contains("Launcher " + Surface.LauncherVersion) && window.Title.Contains("Game "), window.Title);
                Shot("live_01_play");
            });
            then(200, () =>
            {
                bool none = Ini("Game", "FrameLock") == null;
                Key(System.Windows.Input.Key.S, ModifierKeys.Control);
                Check("a save writes the frame rate the launcher shows though it is the default: the file had no live FrameLock line, now FrameLock=1",
                    none && Ini("Game", "FrameLock") == "1" && !model.AnyChanged, "before: " + (none ? "no line" : "a line") + ", after: " + Ini("Game", "FrameLock"));
            });
            then(200, () =>
            {
                Key(System.Windows.Input.Key.Down);          // the first key only shows the focus
                Key(System.Windows.Input.Key.Down);
                Key(System.Windows.Input.Key.Enter);
                Check("keyboard: Down, Down, Enter on the rail opens Display", ui.PageName == "Display", ui.PageName);
            });
            then(300, () =>
            {
                Key(System.Windows.Input.Key.Right);         // into the card: the Screen row
                Key(System.Windows.Input.Key.Right);         // Window -> Full screen
                Check("keyboard: Right, Right sets Full screen", model.Get(S.Fullscreen) == "true" && model.IsChanged(S.Fullscreen), model.Get(S.Fullscreen));
                Click(ui.PartRect(S.FrameLock.Id, 0));
                Check("mouse: a click on 30 fps sets FrameLock=2", model.Get(S.FrameLock) == "2", model.Get(S.FrameLock));
                Shot("live_02_display_changed");
            });
            then(200, () =>
            {
                Key(System.Windows.Input.Key.S, ModifierKeys.Control);
                Check("Ctrl+S writes the file", Ini("Rendering", "Fullscreen") == "true" && Ini("Game", "FrameLock") == "2" && !model.AnyChanged);
                Check("and says so", ui.MessageText == "Settings saved.", ui.MessageText);
            });
            then(200, () =>
            {
                ui.ShowPage("Controls");
                ui.FocusRow("bind:Square", 0);
            });
            then(300, () =>
            {
                Key(System.Windows.Input.Key.Enter);
                Check("Enter on a chip starts the capture", ui.Capture != null);
                Key(System.Windows.Input.Key.H);
                Check("the next key is taken: Shoot = H", ui.Capture == null && model.KeysOf("Square").SequenceEqual(new[] { "H" }), string.Join(",", model.KeysOf("Square")));
                Key(System.Windows.Input.Key.Enter);
                Key(System.Windows.Input.Key.W);
                Check("a key another action holds asks first", ui.HasDialog && model.KeysOf("MoveForward").Contains("W"));
                Shot("live_03_conflict");
                Key(System.Windows.Input.Key.Enter);         // Move
                Check("Move: W leaves Move forward and becomes Shoot", !ui.HasDialog && !model.KeysOf("MoveForward").Contains("W") && model.KeysOf("Square").Contains("W"),
                    "MoveForward=" + string.Join(",", model.KeysOf("MoveForward")) + " Square=" + string.Join(",", model.KeysOf("Square")));
            });
            then(300, () =>
            {
                Key(System.Windows.Input.Key.Enter);
                Key(System.Windows.Input.Key.Escape);
                Check("Escape can be bound (the capture takes the first key)", model.KeysOf("Square").Contains("Escape") && model.KeysOf("Start").Count == 0 || ui.HasDialog);
                if (ui.HasDialog) Key(System.Windows.Input.Key.Escape);   // Keep it there
                Check("Escape closes the question with 'Keep'", !ui.HasDialog && model.KeysOf("Start").Contains("Escape"));
            });
            then(300, () =>
            {
                // The mouse: click the chip, then right-click the waiting chip.
                Click(ui.PartRect("bind:Square", 0));
                Check("a click on a chip starts the capture", ui.Capture != null);
            });
            then(200, () =>
            {
                Click(ui.PartRect("bind:Square", 0), MouseButton.Middle);
                Check("a middle click on the waiting chip asks to move Middle click from Weapon wheel", ui.HasDialog);
                Key(System.Windows.Input.Key.Left);
                Key(System.Windows.Input.Key.Left);
                Key(System.Windows.Input.Key.Enter);
                Check("after the question the binding is where the player chose", !ui.HasDialog);
                Note("Mouse3 is now " + model.MouseTarget("Mouse3") + ", Shoot keys " + string.Join(",", model.KeysOf("Square")));
            });
            int wholeAtWait = 0, liveAtWait = 0;
            then(200, () =>
            {
                ui.FocusRow("bind:Walk", 0);
                Key(System.Windows.Input.Key.Enter);
                Check("capture started for the time-out test", ui.Capture != null);
            });
            then(700, () => { wholeAtWait = ui.Renders; liveAtWait = ui.LiveRenders; });
            then(1000, () =>
            {
                Check("while a chip waits for a key only its outline and its count-down are drawn again, not the whole picture",
                    ui.Capture != null && ui.Renders == wholeAtWait && ui.LiveRenders - liveAtWait >= 12,
                    "in one second: whole picture " + (ui.Renders - wholeAtWait) + " times, the waiting chip alone " + (ui.LiveRenders - liveAtWait) + " times");
            });
            then(3900, () =>
            {
                Check("a capture ends by itself after five seconds", ui.Capture == null);
                Check("and nothing was bound", model.KeysOf("Walk").SequenceEqual(new[] { "X" }), string.Join(",", model.KeysOf("Walk")));
            });
            then(200, () =>
            {
                ui.FocusRow("bind:Walk", 0);
                Key(System.Windows.Input.Key.Delete);
                Check("Delete removes the key; the action shows 'none'", model.KeysOf("Walk").Count == 0);
                Shot("live_04_controls");
            });
            then(200, () =>
            {
                var plus = ui.PartRect("bind:Walk", 0);
                Check("an action without a key has one button, the '+' after the dim 'none'", !plus.IsEmpty && plus.Width < 40 && plus.X > Lay.ControlX + 40, plus.ToString());
                Click(plus);
                Check("a click on it starts the capture", ui.Capture != null);
                Key(System.Windows.Input.Key.X);
                Check("and the key is back: Walk = X", ui.Capture == null && model.KeysOf("Walk").SequenceEqual(new[] { "X" }), string.Join(",", model.KeysOf("Walk")));
            });
            // A scrolled list rests on whole rows, however it was moved.
            then(200, () => { ui.ShowPage("Controls"); ui.SetPadView(false); ui.ScrollTo(0); });
            double tabsAt = double.NaN;
            then(300, () =>
            {
                string d;
                Check("Controls opens on whole rows", ui.ListRestsOnWholeRows(out d) && ui.ListScroll < 0.5, d);
                tabsAt = ui.RowShownAt("device");
                ui.PointerWheel(over, -120);
            });
            then(400, () =>
            {
                string d;
                bool whole = ui.ListRestsOnWholeRows(out d);
                Check("one notch of the wheel moves the list onto a whole row", whole && ui.ListScroll > 1, d);
                var tab = ui.PartRect("device", 1);
                Check("the two tabs stay under the title while the list moves under them", !double.IsNaN(tabsAt) && Math.Abs(ui.RowShownAt("device") - tabsAt) < 0.01 && !tab.IsEmpty && tab.Bottom <= ui.ListTop,
                    "tabs at " + ui.RowShownAt("device").ToString("0.#") + ", the list begins at " + ui.ListTop.ToString("0.#"));
                for (int i = 0; i < 4; i++) ui.PointerWheel(over, -120);
            });
            then(400, () =>
            {
                string d;
                Check("four more notches: whole rows again", ui.ListRestsOnWholeRows(out d), d);
                Shot("live_10_wheel");
                double before = ui.ListScroll;
                ui.PointerWheel(over, -40);
                Check("a third of a notch (a smooth wheel) does not move it", Math.Abs(ui.ListScroll - before) < 0.1);
                ui.PointerWheel(over, -40); ui.PointerWheel(over, -40);
                Check("three thirds do", ui.ListScroll > before + 1);
                for (int i = 0; i < 40; i++) ui.PointerWheel(over, -120);
            });
            then(400, () =>
            {
                string d;
                Check("at the end of the list the first and the last row are whole", ui.ListRestsOnWholeRows(out d) && ui.ListScroll > 100, d);
                // The thumb: free while it is dragged, on a row when let go.
                ui.PointerDown(new Point(1231, Lay.RowsBottom - 20), MouseButton.Left);
                ui.PointerMove(new Point(1231, 397));
            });
            then(300, () =>
            {
                ui.PointerUp(new Point(1231, 397));
            });
            then(400, () =>
            {
                string d;
                Check("the thumb let go in the middle: the list settles on a whole row", ui.ListRestsOnWholeRows(out d) && ui.ListScroll > 1, d);
                // The right stick, with made-up controller states.
                window.FakePad = true;
                ui.PadUpdate(new PadState { Connected = true }, true);
                for (int i = 0; i < 9; i++) ui.PadUpdate(new PadState { Connected = true, RY = 24000 }, true);
                ui.PadUpdate(new PadState { Connected = true }, true);
            });
            then(400, () =>
            {
                string d;
                Check("the right stick let go: the list settles on a whole row", ui.ListRestsOnWholeRows(out d), d);
                window.FakePad = false;
                Key(System.Windows.Input.Key.PageUp); Key(System.Windows.Input.Key.PageUp); Key(System.Windows.Input.Key.PageUp);
                Key(System.Windows.Input.Key.PageUp); Key(System.Windows.Input.Key.PageUp); Key(System.Windows.Input.Key.PageUp);
            });
            then(400, () =>
            {
                string d;
                Check("Page Up goes back to the top", ui.ListRestsOnWholeRows(out d) && ui.ListScroll < 0.5, d);
                var more = ui.MoreButton(true);
                Check("the arrow for more below is at the foot of the scrollbar, inside the card", more.X > Lay.RowRight - 6 && more.Right <= Lay.CardX + Lay.CardW + 4 && more.Bottom <= Lay.CardY + Lay.CardH, more.ToString());
                Click(ui.MoreButton(true));
            });
            then(400, () =>
            {
                string d;
                Check("a click on the arrow under the list moves it by a screen, onto whole rows", ui.ListRestsOnWholeRows(out d) && ui.ListScroll > 100, d);
                Click(ui.MoreButton(false));
            });
            then(400, () =>
            {
                string d;
                Check("and the arrow above it moves it back", ui.ListRestsOnWholeRows(out d) && ui.ListScroll < 0.5, d);
            });
            then(200, () =>
            {
                ui.ShowPage("Textures & mods");
            });
            // The fire on the HD pack choice: it stands still while the launcher is not the window in front
            // (this test never takes the focus), and burns when it is.
            then(900, () =>
            {
                Check("the HD pack choice is on fire only in the front window: nothing is drawn while another window is in front",
                    model.TexturePacks.Any(p => p.Enabled) && !ui.WindowActive && !ui.Animating, "active=" + ui.WindowActive + " moving=" + ui.Animating);
                window.FrontOverride = true;
            });
            int wholeBefore = 0, fireBefore = 0;
            then(500, () =>
            {
                Check("in the front window the fire moves", ui.WindowActive && (ui.FireRuns || !SystemParameters.ClientAreaAnimation), "fire=" + ui.FireRuns);
                wholeBefore = ui.Renders; fireBefore = ui.PartRenders;
            });
            then(1000, () =>
            {
                int whole = ui.Renders - wholeBefore, fireOnly = ui.PartRenders - fireBefore;
                Check("while it burns only the fire is drawn again, not the whole picture", whole == 0 && (fireOnly >= 10 || !SystemParameters.ClientAreaAnimation),
                    "in one second: whole picture " + whole + " times, fire alone " + fireOnly + " times");
                ui.ShowPage("Display");
            });
            then(600, () => { wholeBefore = ui.Renders; fireBefore = ui.PartRenders; });
            then(1000, () =>
            {
                Check("on a page without the fire nothing is drawn again, in the front window too", ui.Renders == wholeBefore && ui.PartRenders == fireBefore && !ui.Animating,
                    "in one second: whole picture " + (ui.Renders - wholeBefore) + " times, fire " + (ui.PartRenders - fireBefore) + " times, moving=" + ui.Animating);
                window.FrontOverride = null;
                ui.ShowPage("Textures & mods");
            });
            then(300, () =>
            {
                Click(ui.PartRect("textures", 0));
                Check("Standard textures: every texture pack is off, the other mods stay", model.TexturePacks.All(p => !p.Enabled) && model.Mods.Any(x => x.Enabled),
                    "Enabled=" + model.Get(S.ModsEnabled));
            });
            then(300, () =>
            {
                Click(ui.PartRect("textures", 1));
                Check("HD textures: one pack is on", model.TexturePacks.Count(p => p.Enabled) == 1, "Enabled=" + model.Get(S.ModsEnabled));
            });
            then(300, () =>
            {
                ui.OpenDropdownFor("pack");
                Check("the pack list opens", ui.HasDropdown);
                Shot("live_05_dropdown");
                Key(System.Windows.Input.Key.Down);
                Key(System.Windows.Input.Key.Enter);
                var on = model.TexturePacks.Where(p => p.Enabled).Select(p => p.Name).ToList();
                Check("Down, Enter chooses the second pack", !ui.HasDropdown && on.Count == 1 && on[0] == model.TexturePacks[1].Name, string.Join(",", on));
                Key(System.Windows.Input.Key.S, ModifierKeys.Control);
                Check("the mods line is written", (Ini("Mods", "Enabled") ?? "").Contains(on.Count > 0 ? on[0] : "?"), "Enabled=" + Ini("Mods", "Enabled"));
            });
            then(300, () =>
            {
                ui.ShowPage("Display");
                Click(ui.DefaultsButton);
                Check("Defaults asks first", ui.HasDialog);
                Key(System.Windows.Input.Key.Enter);         // the focus is on Cancel
                Check("Enter on the question's default (Cancel) changes nothing", !ui.HasDialog && model.Get(S.Fullscreen) == "true");
                Click(ui.DefaultsButton);
                Key(System.Windows.Input.Key.Left);
                Key(System.Windows.Input.Key.Enter);
                Check("Reset puts the page's settings back", model.Get(S.Fullscreen) == "false" && model.Get(S.FrameLock) == "1", model.Get(S.Fullscreen) + " " + model.Get(S.FrameLock));
            });
            // A failed save: one line, and what Windows said behind a button.
            then(200, () =>
            {
                ui.SayCannotWrite("The file is in use");
                Check("a failed save is one short sentence with a Details button", ui.MessageText == Surface.CannotWrite && !ui.MessageText.Contains(". ") && !ui.MessageText.Contains("?") && ui.MessageButton == "Details", ui.MessageText);
                Drawn();
                Check("and the warning triangle stands in front of it, as with every problem", ui.MessageHasTriangle);
                ui.PressMessageButton();
                Check("Details shows what Windows said", ui.HasDialog);
                Key(System.Windows.Input.Key.Enter);
                Check("and Enter closes it", !ui.HasDialog);
                Key(System.Windows.Input.Key.F5);
            });
            then(600, () =>
            {
                Check("F5 looks again and says nothing: the standing error keeps its place and its button", ui.MessageText == Surface.CannotWrite && ui.MessageButton == "Details", ui.MessageText);
                ui.Say("", Pal.Cream);
            });
            // Quality is a look; the limit on the automatic resolution is its own row.
            then(200, () => ui.ShowPage("Graphics"));
            then(300, () =>
            {
                string d;
                Check("Graphics needs no scrolling, and Camera is not on it", !ui.ListScrolls && ui.ListRestsOnWholeRows(out d) && ui.FindRow(S.Camera.Id) == null);
                Check("Quality's 'Custom' is not drawn while a quality applies", ui.PartRect("quality", 4).IsEmpty && !ui.PartRect("quality", 3).IsEmpty);
                Click(ui.PartRect(S.Anisotropy.Id, 3));      // 8x
            });
            then(300, () =>
            {
                Check("one of its rows changed: 'Custom' is drawn", !ui.PartRect("quality", 4).IsEmpty && model.Get(S.Anisotropy) == "8", model.Get(S.Anisotropy));
            });
            then(300, () =>
            {
                Click(ui.PartRect("quality", 0));            // Original
                Check("Quality 'Original' turns anti-aliasing off and does not write [Rendering] Preset", ui.AaNow() == "off" && !model.IsChanged(S.Preset),
                    "aa=" + ui.AaNow() + " Preset=" + model.Get(S.Preset));
                Click(ui.PartRect("quality", 2));            // High
                Check("Quality 'High' puts the looks back", ui.AaNow() == "fxaa" && model.Get(S.Anisotropy) == "16", "aa=" + ui.AaNow());
                ui.ShowPage("Display");
                ui.OpenAdvanced("Display");
            });
            then(300, () =>
            {
                Check("Display with Advanced open needs no scrolling", ui.PartRect(S.Fullscreen.Id, 0).Top > Lay.RowsTop && ui.PartRect(S.Preset.Id, 0).Bottom < Lay.RowsBottom,
                    ui.PartRect(S.Fullscreen.Id, 0).Top.ToString("0") + " to " + ui.PartRect(S.Preset.Id, 0).Bottom.ToString("0"));
                Click(ui.PartRect(S.Preset.Id, 2));          // 960 x 544
                Check("'Auto resolution limit: 960 x 544' sets Preset=low", model.Get(S.Preset) == "low", model.Get(S.Preset));
                Click(ui.PartRect(S.InternalScale.Id, 2));   // 2x
                Check("with a fixed resolution that row is locked", !ui.FindRow(S.Preset.Id).IsEnabled(model));
                Click(ui.PartRect(S.InternalScale.Id, 0));   // Auto
                Click(ui.PartRect(S.Preset.Id, 0));          // No limit
                Check("'No limit' puts it back", !model.IsChanged(S.Preset) && !model.IsChanged(S.InternalScale), model.Get(S.Preset) + " " + model.Get(S.InternalScale));
                ui.ShowPage("Controls");
                ui.SetPadView(false);
            });
            then(300, () =>
            {
                var turn = ui.PartRect(S.SensitivityX.Id, 0);
                Check("Controls: the mouse's turn speed is on the first screen, above the bindings", !turn.IsEmpty && turn.Top > Lay.RowsTop && turn.Bottom < Lay.RowsBottom,
                    turn.IsEmpty ? "not drawn" : turn.Top.ToString("0"));
                Check("Camera is on Controls, under Aiming", ui.FindRow(S.Camera.Id) != null);
                Click(ui.PartRect("device", 1));             // the Gamepad tab
            });
            then(300, () =>
            {
                var turn = ui.PartRect(S.TurnSensitivity.Id, 0);
                Check("the Gamepad tab shows the stick's turn speed on the first screen", !turn.IsEmpty && turn.Top > Lay.RowsTop && turn.Bottom < Lay.RowsBottom,
                    turn.IsEmpty ? "not drawn" : turn.Top.ToString("0"));
                Check("and Camera on the Gamepad tab too", ui.FindRow(S.Camera.Id) != null);
                ui.FocusRow("bind:MoveForward", 0);
            });
            then(300, () =>
            {
                var plus = ui.PartRect("bind:MoveForward", 0);
                ClickAt(Lay.ControlX + 20, plus.Y + plus.Height / 2);
                Check("the dimmed 'Left stick' chip of Move forward is no button; a click on it says why", ui.ReportText == "Move forward: Always on the left stick.", ui.ReportText + " / " + plus);
                ui.Say("", Pal.Cream);
                ui.SetPadView(false);
            });
            // ---- no explanatory text; what is locked says why only when it is tried ----
            then(200, () => { window.FakePad = true; ui.PadUpdate(new PadState(), true); ui.Say("", Pal.Cream); ui.ShowPage("Play"); });
            then(300, () =>
            {
                var texts = Drawn();
                Check("Play: the three key chips are captioned with action names", texts.Contains("Full screen") && texts.Contains("Free the mouse") && texts.Contains("Pause"), string.Join(" | ", texts.Where(t => t.Length < 20)));
                var longest = texts.Where(t => t.Contains(". ") || t.EndsWith(".")).ToList();
                Check("Play: no sentence is drawn on the page as it stands", longest.Count == 0, string.Join(" | ", longest));
                Check("the Ready strip counts the mods that will load: the empty folder is not one of them", texts.Any(t => t.Contains("HD textures") && t.Contains("2 mods")), string.Join(" | ", texts.Where(t => t.Contains("mod"))));
                model.Disc = new DiscInfo();
                ui.Rebuild();
            });
            then(300, () =>
            {
                var texts = Drawn();
                Check("'Choose your disc': the title and the two buttons, no paragraph between them",
                    texts.Contains("Choose your disc") && texts.Contains("Choose .iso...") && ui.FindRow("needdisc") == null && !texts.Any(t => t.Contains("Nothing is copied") || t.Contains("your own copy")));
                model.RefreshDisc();
                ui.ShowPage("Controls");
                ui.SetPadView(true);
            });
            then(300, () =>
            {
                var texts = Drawn();
                Check("Gamepad tab without a controller: 'No controller found' and nothing else about it", texts.Contains("No controller found") && ui.FindRow("padstatus") != null && !texts.Any(t => t.StartsWith("Controller connected")));
                Pad(0);
            });
            then(300, () =>
            {
                Check("with a controller connected the header says nothing", ui.FindRow("padstatus") == null && !Drawn().Any(t => t.Contains("ontroller found") || t.StartsWith("Controller connected")));
                ui.FocusRow(S.DeadzoneLeft.Id);
            });
            then(300, () =>
            {
                var texts = Drawn();
                Check("no 'Left' / 'Right' beside the stick dials", texts.Contains("Stick test") && !texts.Contains("Left") && !texts.Contains("Right"));
                ui.PadUpdate(new PadState(), true);
                ui.SetPadView(false);
                ui.ShowPage("Textures & mods");
                ui.FocusRow("mod:work_in_progress", 0);
            });
            then(300, () =>
            {
                var texts = Drawn();
                Check("an empty mod folder is its name alone, locked, with the padlock",
                    texts.Contains("work_in_progress") && !texts.Any(t => t.Contains("(empty)")) && ui.IsRowLocked("mod:work_in_progress") && !ui.RowPadlock("mod:work_in_progress").IsEmpty && ui.RowPadlock("mod:pc_camera").IsEmpty);
                var mod = model.Mods.First(x => x.Name == "work_in_progress");
                bool was = mod.Enabled;
                Click(ui.PartRect("mod:work_in_progress", 0));
                Check("its switch does nothing; the click says why", mod.Enabled == was && ui.ReportText == "work_in_progress: This folder is empty.", ui.ReportText);
                var keep = model.Mods.ToList();
                model.Mods.Clear();
                ui.Rebuild();
                texts = Drawn();
                Check("with no mods at all: no heading and no sentence, the page goes on with the mods folder",
                    ui.FindRow("group:Installed mods") == null && ui.FindRow("nomods") == null && ui.FindRow("modsfolder") != null && !texts.Any(t => t.StartsWith("No mods")));
                model.Mods.AddRange(keep.Where(x => !x.HasTextures));
                ui.ShowPage("Play");
                ui.ShowPage("Textures & mods");
            });
            then(300, () =>
            {
                Check("a click on a choice that is dimmed says that choice's reason", !ui.IsRowLocked("textures") && ui.ReportText == "", ui.ReportText);
                Click(ui.PartRect("textures", 1));
                Check("... 'HD pack' without a pack: 'No HD pack found.'", ui.ReportText == "Textures: No HD pack found." && ui.RowPadlock("textures").IsEmpty, ui.ReportText);
                ui.ShowPage("Sound");
                ui.ShowPage("Textures & mods");
                ui.FocusRow("textures");
                Key(System.Windows.Input.Key.Enter);
                Check("Enter on that row has no other choice to go to: it says the dimmed choice's reason too", ui.ReportText == "Textures: No HD pack found." && model.TexturePacks.Count == 0, ui.ReportText);
                Drawn();
                Check("and the prompts do not offer a next choice there", ui.Device == InputDevice.Keyboard && !ui.PromptLog.Contains("Next choice") && !ui.PromptLog.Contains("key:Enter"), ui.PromptLog);
                ui.ShowPage("Sound");
                ui.ShowPage("Textures & mods");
                ui.FocusRow("textures");
                Key(System.Windows.Input.Key.Right);
                Check("and so does the Right key, which runs into it", ui.ReportText == "Textures: No HD pack found.", ui.ReportText);
                model.ScanMods();
                ui.ShowPage("Display");
                ui.OpenAdvanced("Display");
                model.Set(S.InternalScale, "2");
                ui.Say("", Pal.Cream);
            });
            then(300, () =>
            {
                Check("a locked row shows a padlock after its name, an open one does not",
                    ui.IsRowLocked(S.Preset.Id) && !ui.RowPadlock(S.Preset.Id).IsEmpty && ui.RowPadlock(S.Preset.Id).X > Lay.LabelX + 100 && ui.RowPadlock(S.Fullscreen.Id).IsEmpty, ui.RowPadlock(S.Preset.Id).ToString());
                ui.FocusRow(S.EarlyPresent.Id);
                Key(System.Windows.Input.Key.Down);          // the Advanced divider
                Key(System.Windows.Input.Key.Down);          // the locked row
                Check("the keyboard's focus lands on a locked row, and moving there says nothing", ui.FocusedRow == S.Preset.Id && ui.ReportText == "" && ui.MessageText == "", ui.FocusedRow + " / " + ui.MessageText);
                Key(System.Windows.Input.Key.Right);
                Check("its control does nothing", model.Get(S.Preset) == "high" && ui.ReportText == "", model.Get(S.Preset));
                Key(System.Windows.Input.Key.Enter);
                Check("Enter on it says why", ui.ReportText == "Auto resolution limit: Only used with Auto resolution." && model.Get(S.Preset) == "high", ui.ReportText);
                Shot("live_11_locked_row");
                Key(System.Windows.Input.Key.Up);
                Check("the report goes when the focus leaves the row", ui.ReportText == "" && ui.MessageText == "", ui.MessageText);
            });
            then(300, () =>
            {
                // A standing error with a button; a report is laid over it and it comes back.
                ui.SayCannotWrite("The file is in use");
                var name = new Point(Lay.LabelX + 30, ui.RowShownAt(S.Preset.Id) + 26);
                ui.PointerMove(name); ui.PointerDown(name, MouseButton.Left); ui.PointerUp(name);
                Check("a click on a locked row's name says why", ui.ReportText.StartsWith("Auto resolution limit:") && ui.MessageText == ui.ReportText, ui.MessageText);
                Check("the error under it is not destroyed, and its button waits", ui.StandingMessage.StartsWith("Could not write") && ui.MessageButton == null, ui.StandingMessage);
                Click(ui.PartRect(S.EarlyPresent.Id, 0));
                Click(ui.PartRect(S.EarlyPresent.Id, 0));
                Check("a click on a row that is not locked says nothing", ui.ReportText == "");
            });
            then(300, () =>
            {
                Check("when the report has gone the error is back with its button", ui.MessageText.StartsWith("Could not write") && ui.MessageButton == "Details", ui.MessageText);
                Pad(0);
                ui.FocusRow(S.Preset.Id);
                Pad(Surface.PadA); Pad(0);
                Check("gamepad: A on a locked row says why", ui.ReportText.StartsWith("Auto resolution limit:"), ui.ReportText);
                Drawn();
                Check("and the prompts along the bottom offer nothing for A there", !ui.PromptLog.Contains("xbox:A "), ui.PromptLog);
                ui.ShowPage("Play");
                Check("the report goes with the page; the error stays", ui.ReportText == "" && ui.MessageText.StartsWith("Could not write"), ui.MessageText);
                ui.PadUpdate(new PadState(), true);
            });
            then(300, () =>
            {
                Click(ui.PartRect("direct", 0));
                Check("'Skip this window next time' on: launcher.ini has it, and the standing error keeps its place", ui.MessageText.StartsWith("Could not write") && ui.ReportText == "" && model.Config.GetBool("StartGameDirectly", false), ui.MessageText);
                Click(ui.PartRect("direct", 0));
                ui.Say("", Pal.Cream);
                Click(ui.PartRect("direct", 0));
                var texts = Drawn();
                Check("with no error standing: switching it on says nothing at all (no sentence about getting the window back)",
                    model.Config.GetBool("StartGameDirectly", false) && ui.MessageText == "" && ui.ReportText == "" && !texts.Any(t => t.Contains("Shift") || t.Contains("this window again")), ui.MessageText);
            });
            then(300, () =>
            {
                Click(ui.PartRect("direct", 0));
                Check("(the switch is off again)", !model.Config.GetBool("StartGameDirectly", false));
                ui.Say("", Pal.Cream);
                model.Set(S.InternalScale, "auto");
                ui.ShowPage("Controls");
                ui.SetPadView(true);
            });
            // ---- the four new rows, for a game that has the keys ----
            then(300, () =>
            {
                Check("Controls has the rows for the on-screen prompts, the button pictures, PlayStation controllers and Rumble (the stand-in game holds their keys)",
                    ui.FindRow(S.Prompts.Id) != null && ui.FindRow(S.PadGlyphs.Id) != null && ui.FindRow(S.PlayStationPads.Id) != null && ui.FindRow(S.Rumble.Id) != null);
                Check("no heading is repeated by the row under it: Controller / Which controller, Prompts / On screen and Button pictures; a switch's name does not hold its state (Use the gamepad)",
                    ui.FindRow(S.PadIndex.Id).Label == "Which controller" && ui.FindRow(S.PlayStationPads.Id).Label == "Use PlayStation controllers" && ui.FindRow(S.PadEnabled.Id).Label == "Use the gamepad"
                    && ui.FindRow(S.Prompts.Id).Label == "On screen" && ui.FindRow(S.PadGlyphs.Id).Label == "Button pictures");
                ui.SetPadView(false);
                Check("the two rows under Prompts are on the Keyboard & mouse tab too", ui.FindRow(S.Prompts.Id) != null && ui.FindRow(S.PadGlyphs.Id) != null && ui.FindRow(S.Rumble.Id) == null);
                var real = model.Game;
                model.Game = GameInfo.Knowing(real.ExePath, real.Known.Where(p => !p.StartsWith("Prompts") && !p.StartsWith("Glyphs") && p != "PlayStationPads" && p != "Rumble"));
                ui.SetPadView(true);
                Check("a game without those keys: the rows are not there, nor the empty 'Prompts' heading",
                    ui.FindRow(S.Prompts.Id) == null && ui.FindRow(S.PadGlyphs.Id) == null && ui.FindRow(S.PlayStationPads.Id) == null && ui.FindRow(S.Rumble.Id) == null && ui.FindRow("group:Prompts") == null && ui.FindRow("group:Controller") != null);
                model.Game = real;
                ui.Rebuild();
                ui.FocusRow(S.Prompts.Id);
            });
            then(300, () =>
            {
                Click(ui.PartRect(S.Prompts.Id, 2));
                Check("On screen: Gamepad writes [Input] Prompts=gamepad", model.Get(S.Prompts) == "gamepad", model.Get(S.Prompts));
                ui.FocusRow(S.PadGlyphs.Id);
            });
            then(300, () =>
            {
                Click(ui.PartRect(S.PadGlyphs.Id, 2));
                Check("Button pictures: PlayStation writes [Input.Gamepad] Glyphs=playstation", model.Get(S.PadGlyphs) == "playstation", model.Get(S.PadGlyphs));
                Check("and the launcher draws its own controller pictures that way at once, with no controller", ui.Look == PadKind.PlayStation);
                ui.FocusRow(S.PlayStationPads.Id);
            });
            then(300, () =>
            {
                Click(ui.PartRect(S.PlayStationPads.Id, 0));
                Check("Use PlayStation controllers: the switch writes PlayStationPads=false", model.Get(S.PlayStationPads) == "false", model.Get(S.PlayStationPads));
                var heard = window.Listen(new PadState(), PadTests.State(0x0CE6, PsCross, 64), 1000);
                Check("switched off, the launcher itself still listens to a PlayStation pad (the switch is the game's key), so the pad can switch it on again",
                    heard.Connected && heard.Kind == PadKind.PlayStation && heard.Buttons == Surface.PadA, "buttons " + heard.Buttons.ToString("X4"));
                ui.FocusRow(S.Rumble.Id);
                Key(System.Windows.Input.Key.Left); Key(System.Windows.Input.Key.Left);
                Check("Rumble: the slider is in percent, two steps down is 0.9", model.Get(S.Rumble) == "0.9", model.Get(S.Rumble));
            });
            double shaken = -1;
            then(300, () =>
            {
                ui.RumbleRequested = strength => shaken = strength;
                Click(ui.PartRect(S.Rumble.Id, 1));
                Check("Rumble's Test without a controller does nothing and says why", shaken < 0 && ui.ReportText == "Rumble: No controller found.", ui.ReportText);
                Shot("live_12_controller_rows");
                Pad(0);
                Drawn();
                Click(ui.PartRect(S.Rumble.Id, 1));
                Check("with an Xbox pad it asks for a shake as strong as the slider says", Math.Abs(shaken - 0.9) < 1e-9, shaken.ToString());
                shaken = -1;
                Pad(Surface.PadDown); Pad(0);               // the pad drives now
                ui.FocusRow(S.Rumble.Id);
                Pad(Surface.PadA); Pad(0);
                Check("and A on the row presses Test", Math.Abs(shaken - 0.9) < 1e-9, shaken.ToString());
                Drawn();
                Check("the prompts for a slider: the D-pad's left and right with 'Change', and A with 'Test'", ui.PromptLog.Contains(":DpadLR Change") && ui.PromptLog.Contains(":A Test"), ui.PromptLog);
                shaken = -1;
                PsReport(PsRest);
                Drawn();
                Click(ui.PartRect(S.Rumble.Id, 1));
                Check("with a PlayStation pad Test is dimmed and says why", shaken < 0 && ui.ReportText == "Rumble: The test works with an Xbox controller.", ui.ReportText);
                Key(System.Windows.Input.Key.S, ModifierKeys.Control);
                Check("the four keys are written: Prompts=gamepad, Glyphs=playstation, PlayStationPads=false, Rumble=0.9",
                    Ini("Input", "Prompts") == "gamepad" && Ini("Input.Gamepad", "Glyphs") == "playstation" && Ini("Input.Gamepad", "PlayStationPads") == "false" && Ini("Input.Gamepad", "Rumble") == "0.9",
                    Ini("Input", "Prompts") + " " + Ini("Input.Gamepad", "Glyphs") + " " + Ini("Input.Gamepad", "PlayStationPads") + " " + Ini("Input.Gamepad", "Rumble"));
            });
            // ---- the pictures follow the controller and the device used last ----
            then(300, () =>
            {
                model.Set(S.PadGlyphs, "auto");
                Pad(0);
                Pad(Surface.PadDown); Pad(0);
                Drawn();
                Check("Button pictures on Automatic with an Xbox pad used last: Xbox pictures along the bottom", ui.Look == PadKind.Xbox && ui.PromptLog.StartsWith("xbox:"), ui.PromptLog);
                PsReport(PsRest);
                PsReport("01 80 80 80 80 00 00 00 04 00 00"); PsReport(PsRest);      // D-pad down
                var texts = Drawn();
                Check("a PlayStation pad used last: PlayStation pictures along the bottom", ui.Look == PadKind.PlayStation && ui.PromptLog.StartsWith("ps:") && ui.Device == InputDevice.Pad, ui.PromptLog);
                ui.FocusRow("bind:Cross", 0);
            });
            then(300, () =>
            {
                var texts = Drawn();
                Check("the binding chips name the buttons as a PlayStation pad does (Cross, Triangle), by position", texts.Contains("Cross") && texts.Contains("Triangle") && !texts.Contains("Left bumper"), string.Join(" | ", texts.Where(t => t.Length < 14)));
                Shot("live_13_playstation");
                model.Set(S.PadGlyphs, "xbox");
                Check("Button pictures on Xbox overrides the controller's family", ui.Look == PadKind.Xbox && Drawn().Contains("Left bumper"));
                model.Set(S.PadGlyphs, "auto");
                Key(System.Windows.Input.Key.Down);
                Drawn();
                Check("the keyboard used last: key caps along the bottom", ui.Device == InputDevice.Keyboard && ui.PromptLog.StartsWith("key:Enter "), ui.PromptLog);
                ui.PointerWheel(over, -120);
                Drawn();
                Check("the mouse used last: no prompts", ui.Device == InputDevice.Mouse && ui.PromptLog == "", ui.PromptLog);
                ui.FocusRow("bind:Select", 0);
            });
            then(300, () =>
            {
                // A PlayStation pad's reports, through the decoder, rebind an action.
                PsReport(PsRest);
                PsReport(PsCross);
                Check("PlayStation pad: Cross on a chip starts the capture", ui.Capture != null);
                Drawn();
                Check("while the controller waits for its own button the strip along the bottom shows that player nothing", ui.Device == InputDevice.Pad && ui.PromptLog == "", ui.PromptLog);
                ui.SetDevice(InputDevice.Keyboard);
                Drawn();
                Check("with the keyboard used last it shows the Esc key cap and 'Cancel'", ui.PromptLog == "key:Esc Cancel", ui.PromptLog);
                ui.SetDevice(InputDevice.Pad);
                PsReport(PsRest);
                PsReport(PsL3);
                Check("L3 is taken: it holds another action, so the question names it as the pad does", ui.HasDialog && ui.DialogText.StartsWith("L3 is already "), ui.DialogText);
                PsReport(PsRest);
                PsReport(PsCircle); PsReport(PsRest);
                Check("Circle answers 'Keep'", !ui.HasDialog && model.PadTarget("LS") == "L" && model.PadTarget("Back") == "Select", model.PadTarget("LS") + " " + model.PadTarget("Back"));
                PsReport(PsCross); PsReport(PsRest);
                PsReport("01 80 80 80 80 00 00 00 08 80 00");                         // R3, bound to nothing
                Check("R3 (free) becomes Select: stored by position as RS", ui.Capture == null && model.PadTarget("RS") == "Select" && model.PadTarget("Back") == "None", "RS=" + model.PadTarget("RS") + " Back=" + model.PadTarget("Back"));
                PsReport(PsRest);
                model.ResetPadBindings();
                model.Set(S.Prompts, "auto"); model.Set(S.PlayStationPads, "true"); model.Set(S.Rumble, "1");
                Key(System.Windows.Input.Key.S, ModifierKeys.Control);
                ui.PadUpdate(new PadState(), true);
                ui.SetPadView(false);
                ui.ShowPage("Display");
            });
            // ---- the focus ring breathes after the controller, in its own visual ----
            int ringBefore = 0, wholeAtRing = 0;
            then(300, () =>
            {
                window.FrontOverride = true;
                Pad(0);
                Pad(Surface.PadRight); Pad(0);
                Pad(Surface.PadDown); Pad(0);
            });
            then(600, () => { ringBefore = ui.RingRenders; wholeAtRing = ui.Renders; });
            then(1000, () =>
            {
                Check("after the controller the focus ring breathes: only the ring is drawn again, not the whole picture",
                    ui.Renders == wholeAtRing && (ui.RingRenders - ringBefore >= 8 || !SystemParameters.ClientAreaAnimation) && (ui.RingBreathes || !SystemParameters.ClientAreaAnimation),
                    "in one second: whole picture " + (ui.Renders - wholeAtRing) + " times, ring alone " + (ui.RingRenders - ringBefore) + " times");
                window.FrontOverride = null;
                ui.PadUpdate(new PadState(), true);
                Key(System.Windows.Input.Key.Escape);
                window.FakePad = false;
            });
            // The gamepad, with made-up controller states fed where XInput's would go.
            then(200, () =>
            {
                window.FakePad = true;
                ui.ShowPage("Display");
                ui.FocusRail(1);
                Pad(0);
                Pad(Surface.PadRB); Pad(0);
                Check("gamepad: RB goes to the next page", ui.PageName == "Graphics", ui.PageName);
                Pad(Surface.PadLB); Pad(0);
                Check("gamepad: LB goes back", ui.PageName == "Display", ui.PageName);
                model.Set(S.Fullscreen, "false");
                Pad(Surface.PadRight); Pad(0);
                Pad(Surface.PadRight); Pad(0);
                Check("gamepad: right, right enters the card and picks Full screen", model.Get(S.Fullscreen) == "true", model.Get(S.Fullscreen));
                Pad(Surface.PadB); Pad(0);
                Key(System.Windows.Input.Key.Escape);
                Check("keyboard: Escape on the rail does nothing", ui.PageName == "Display", ui.PageName);
                Pad(Surface.PadB); Pad(0);
                Check("gamepad: B returns to the rail, B again goes to Play", ui.PageName == "Play", ui.PageName);
                ui.ShowPage("Controls");
                ui.SetPadView(true);
                ui.FocusRow("bind:Triangle", 0);
            });
            then(300, () =>
            {
                Shot("live_07_gamepad");
                Pad(Surface.PadA);
                Check("gamepad: A on a chip starts the capture", ui.Capture != null);
                Pad(Surface.PadA);
                Check("the A that started it is not taken as the new button", ui.Capture != null);
                Pad(0);
                Pad(Surface.PadY);
                Check("Y is already Jump: the question comes up", ui.HasDialog && ui.Capture == null);
                Pad(0);
                Pad(Surface.PadA); Pad(0);
                Check("A answers Move: Y is Reload, X is free", model.PadTarget("Y") == "Triangle" && model.PadTarget("X") == "None", "Y=" + model.PadTarget("Y") + " X=" + model.PadTarget("X"));
                ui.FocusRow("bind:Square", 0);
            });
            then(300, () =>
            {
                Pad(Surface.PadA); Pad(0);
                Pad(0, 255);
                Check("a trigger pull is taken as a button (Left trigger is Headshot mode: question)", ui.HasDialog);
                Pad(0);
                Pad(Surface.PadB); Pad(0);
                Check("B answers Keep", !ui.HasDialog && model.PadTarget("LT") == "R" && model.PadTarget("RT") == "Square");
                Pad(Surface.PadX); Pad(0);
                Check("gamepad: X on a chip removes that binding", model.PadTarget("RT") == "None", "RT=" + model.PadTarget("RT"));
                ui.SetPadView(false);
                ui.FocusRow("bind:Square", 0);
            });
            then(300, () =>
            {
                Key(System.Windows.Input.Key.Enter);
                Check("keyboard capture started", ui.Capture != null);
                Pad(Surface.PadB); Pad(0);
                Check("the gamepad's B cancels a keyboard capture", ui.Capture == null);
                window.FakePad = false;
            });
            // ---- the prompts point at something, and say what the button does ----
            then(300, () =>
            {
                window.FakePad = true;
                ui.ShowPage("Play");
                ui.FocusRail(0);
                Pad(0);
                Pad(Surface.PadDown); Pad(0);       // the pad drives: the rail's focus goes to Display
                Drawn();
                Check("on the rail, another page under the focus: A opens it; B is not offered on the Play page itself", ui.PromptLog.Contains("xbox:A Open page") && !ui.PromptLog.Contains("Play page"), ui.PromptLog);
                Pad(Surface.PadA); Pad(0);
                Drawn();
                Check("on the open page's own entry A has nothing to do and is not offered; B offers the Play page", ui.PageName == "Display" && !ui.PromptLog.Contains(":A ") && ui.PromptLog.Contains("xbox:B Play page"), ui.PromptLog);
                ui.ShowPage("Game data");
                ui.FocusRow("saves", 1);
            });
            then(300, () =>
            {
                Drawn();
                Check("on a button the prompt for A is the button's own name, and X is not offered for the same thing", ui.PromptLog.Contains("xbox:A Open") && !ui.PromptLog.Contains("xbox:X ") && !ui.PromptLog.Contains("Select"), ui.PromptLog);
                ui.FocusRow(S.DiscPath.Id, 1);
            });
            then(300, () =>
            {
                Drawn();
                Check("'Choose .iso' on that button, and X opens the folder", ui.PromptLog.Contains("xbox:A Choose .iso") && ui.PromptLog.Contains("xbox:X Open"), ui.PromptLog);
                ui.PadUpdate(new PadState(), true);
                Drawn();
                Check("the controller is unplugged: its pictures go from the strip", ui.Device == InputDevice.None && ui.PromptLog == "", ui.Device + " / " + ui.PromptLog);
                // Back from the game with a button still down: that is not a press here.
                Pad(0);
                ui.ShowPage("Play");
                ui.FocusRow("direct");
                bool skip = model.Config.GetBool("StartGameDirectly", false);
                ui.WaitForPadRelease();
                Pad(Surface.PadA);
                Check("a button that is down when the launcher comes back from the game is not a press", model.Config.GetBool("StartGameDirectly", false) == skip);
                Pad(0); Pad(Surface.PadA); Pad(0);
                Check("let go and pressed again, it is one", model.Config.GetBool("StartGameDirectly", false) != skip);
                Pad(Surface.PadA); Pad(0);
                Check("(the switch is as it was)", model.Config.GetBool("StartGameDirectly", false) == skip);
                ui.ShowPage("Controls");
                ui.SetPadView(true);
                ui.ScrollToRow("group:Sticks and triggers");
            });
            // ---- a stick that moves draws its two dots and nothing else ----
            int wholeAtSticks = 0, liveAtSticks = 0;
            Action<int> wiggle = n =>
            {
                for (int i = 0; i < n; i++)
                {
                    double a = i * 0.21;
                    ui.PadUpdate(new PadState { Connected = true, LX = (short)(12000 * Math.Sin(a)), LY = (short)(12000 * Math.Cos(a)), RX = (short)(30000 * Math.Sin(a * 1.3)), RY = (short)(8000 * Math.Cos(a * 1.3)) }, true);
                }
            };
            then(500, () =>
            {
                Check("(the stick dials are in view)", ui.HasDials && !double.IsNaN(ui.RowShownAt("sticks")));
                wholeAtSticks = ui.Renders; liveAtSticks = ui.LiveRenders;
                wiggle(60);
            });
            then(300, () =>
            {
                Check("sixty positions of the sticks: the two dots are drawn again each time, the whole picture not once",
                    ui.Renders == wholeAtSticks && ui.LiveRenders - liveAtSticks >= 30, "whole picture " + (ui.Renders - wholeAtSticks) + " times, the dots alone " + (ui.LiveRenders - liveAtSticks) + " times");
                ui.ScrollTo(0);
            });
            then(400, () =>
            {
                Check("(the stick dials are out of view)", !ui.HasDials);
                wholeAtSticks = ui.Renders; liveAtSticks = ui.LiveRenders;
                wiggle(60);
            });
            then(300, () =>
            {
                Check("with the dials scrolled out of view a moving stick draws nothing at all", ui.Renders == wholeAtSticks && ui.LiveRenders == liveAtSticks,
                    "whole picture " + (ui.Renders - wholeAtSticks) + " times, the dots " + (ui.LiveRenders - liveAtSticks) + " times");
                ui.PadUpdate(new PadState(), true);
                ui.SetPadView(false);
                window.FakePad = false;
            });
            // ---- every page as it stands: nothing in brackets, no sentence, no version line ----
            then(300, () =>
            {
                var odd = new List<string>();
                int read = 0;
                Action<string> look = where =>
                {
                    for (int k = 0; k < 14; k++)
                    {
                        ui.ScrollTo(k * 230);
                        foreach (var text in Drawn())
                        {
                            read++;
                            if (text.Contains("\\")) continue;      // a path
                            bool sentence = text.Length > 3 && ((text.EndsWith(".") && !text.EndsWith("...")) || text.Contains(". ") || text.EndsWith("?"));
                            if (text.Contains("(") || text.Contains(")") || sentence || text.StartsWith("Launcher ")) odd.Add(where + ": " + text);
                        }
                        if (!ui.ListScrolls) break;
                    }
                };
                ui.Say("", Pal.Cream);
                foreach (var name in new[] { "Play", "Display", "Graphics", "Controls", "Textures & mods", "Sound", "Game data" })
                {
                    ui.ShowPage(name);
                    ui.OpenAdvanced(name);
                    if (name != "Controls") { look(name); continue; }
                    ui.SetPadView(false); look("Controls, keyboard & mouse");
                    ui.SetPadView(true); look("Controls, gamepad");
                    ui.SetPadView(false);
                }
                Check("every page from top to end with Advanced open (" + read + " texts): none in brackets, no sentence, no version line", read > 300 && odd.Count == 0, string.Join(" | ", odd.Distinct().Take(8)));
                ui.ShowPage("Display");
                ui.SaySaved();
            });
            // ---- a message whose time is up fades in its own visual ----
            int wholeAtFade = 0, lineAtFade = 0;
            then(2800, () =>
            {
                Check("(the three-second message still stands)", ui.MessageText == "Settings saved.", ui.MessageText);
                wholeAtFade = ui.Renders; lineAtFade = ui.MessageRenders;
            });
            then(1100, () =>
            {
                Check("a message whose time is up fades and goes: only the line is drawn again meanwhile, not the whole picture",
                    ui.MessageText == "" && ui.Renders == wholeAtFade && ui.MessageRenders - lineAtFade >= 8,
                    "'" + ui.MessageText + "', whole picture " + (ui.Renders - wholeAtFade) + " times, the line alone " + (ui.MessageRenders - lineAtFade) + " times");
            });
            then(3600, () =>
            {
                // Long enough for any line in the message bar to have faded (a row's report stays six seconds).
                Check("nothing animates when nothing moves", !ui.Animating);
            });
            // PLAY with the stand-in game: two seconds, exit code 0. It is pressed on the Textures page with the
            // HD pack choice burning in a launcher that is (for the test) the front window.
            then(200, () =>
            {
                ui.ShowPage("Textures & mods");
                window.FrontOverride = true;
            });
            then(500, () =>
            {
                bool burning = ui.FireRuns || !SystemParameters.ClientAreaAnimation;
                File.WriteAllText(fake, "seconds=2\nexit=0\n");
                Click(ui.PlayButton);
                window.FrontOverride = null;
                Check("PLAY saves and hides the window", !window.IsVisible && !model.AnyChanged && window.GameRunning);
                Check("the fire was burning when PLAY was pressed", burning && model.TexturePacks.Any(p => p.Enabled));
            });
            then(300, () => { wholeBefore = ui.Renders; fireBefore = ui.PartRenders; });
            then(700, () =>
            {
                Check("the window stays hidden while the game runs", !window.IsVisible && window.GameRunning);
                Check("and draws nothing meanwhile (though the fire's page is the one shown)", !ui.Animating && !ui.WindowActive && ui.Renders == wholeBefore && ui.PartRenders == fireBefore,
                    "moving=" + ui.Animating + ", whole picture " + (ui.Renders - wholeBefore) + " times, fire " + (ui.PartRenders - fireBefore) + " times");
            });
            then(2600, () =>
            {
                Check("the window comes back when the game ends", window.IsVisible && !window.GameRunning);
                string logFile = Path.Combine(model.GameDir, "logs", "ChiliConCarnage.log");
                string text = File.Exists(logFile) ? File.ReadAllText(logFile) : "";
                Check("the game was started in its own folder with the launcher's settings file", text.Contains("[start] settings file") && text.Contains("closed by the player"), text.Replace("\r\n", " | "));
                if (model.DeveloperStart)
                {
                    string printed = Path.Combine(model.LogsDir, "last_run.log");
                    string output = File.Exists(printed) ? File.ReadAllText(printed) : "";
                    Check("developer start: the arguments are filled in and the game's output is kept in last_run.log",
                        output.Contains(model.Disc.Boot + " | " + model.Disc.Path + " | --ms0 | " + model.SavesDir), output.Replace("\r\n", " | "));
                    Check("developer start: the settings file is handed over when it is not beside the game", !text.Contains("CHILI_CONFIG=(not set)") || string.Equals(model.IniDir, model.GameDir, StringComparison.OrdinalIgnoreCase));
                }
                Check("no error is shown after a clean exit", ui.LastError == "" && !ui.MessageText.Contains("stopped"), ui.MessageText);
            });
            // PLAY with a game that fails at once.
            then(300, () =>
            {
                File.WriteAllText(fake, "seconds=1\nexit=3\nmessage=Direct3D 12 could not start\n");
                Key(System.Windows.Input.Key.Enter, ModifierKeys.None);   // focus is on PLAY after the click
            });
            then(300, () => { if (!window.GameRunning && window.IsVisible) { ui.FocusBottom(1); Key(System.Windows.Input.Key.Enter); } });
            then(2500, () =>
            {
                Check("a game that fails brings the window back on the Play page", window.IsVisible && ui.PageName == "Play", ui.PageName);
                Check("with one short sentence and the button that opens the logs", ui.MessageText == MainWindow.StoppedAtOnce && ui.MessageButton == "Open logs", ui.MessageText + " [" + ui.MessageButton + "]");
                var failedTexts = Drawn();
                Check("the warning triangle in front of it, and no 'Ready' on the strip under what the game said", ui.MessageHasTriangle && !failedTexts.Contains("Ready"));
                Check("and what the game said", ui.LastError.Contains("Direct3D 12 could not start"), ui.LastError.Replace("\n", " | "));
                Shot("live_06_game_failed");
                try { File.Delete(fake); } catch { }
                ui.ShowPage("Display");
                window.Width = window.MinWidth; window.Height = window.MinHeight;
            });
            then(700, () =>
            {
                Note("smallest window: picture " + ui.ActualWidth.ToString("0") + " x " + ui.ActualHeight.ToString("0"));
                Check("the window cannot be made smaller than 960 x 540 of picture", ui.ActualWidth >= 959 && ui.ActualHeight >= 539);
                Shot("live_08_smallest");
                window.Width = 1500; window.Height = 700;
            });
            then(700, () =>
            {
                Shot("live_09_wide");
            });
            then(300, () =>
            {
                File.WriteAllLines(Path.Combine(outDir, "selftest.txt"), log.Concat(new[] { (log.Count(l => l.StartsWith("PASS")) + " passed, " + failed + " failed") }));
                window.Close();
            });

            var timer = new DispatcherTimer(DispatcherPriority.ApplicationIdle);
            timer.Tick += (s, e) =>
            {
                timer.Stop();
                if (steps.Count == 0) return;
                var step = steps.Dequeue();
                try { step.Value(); }
                catch (Exception ex) { Check("no exception", false, ex.ToString()); }
                if (steps.Count > 0) { timer.Interval = TimeSpan.FromMilliseconds(steps.Peek().Key); timer.Start(); }
            };
            w.ContentRendered += (s, e) => { if (timer.IsEnabled) return; timer.Interval = TimeSpan.FromMilliseconds(steps.Peek().Key); timer.Start(); };
            app.Run(w);
            try { File.WriteAllLines(Path.Combine(outDir, "selftest.txt"), log.Concat(new[] { (log.Count(l => l.StartsWith("PASS")) + " passed, " + failed + " failed") })); } catch { }
            return failed;
        }
    }
}
